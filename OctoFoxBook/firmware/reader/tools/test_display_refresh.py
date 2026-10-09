"""Exercise and validate the safe E-Ink refresh policy over USB serial."""

from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass

import serial


REFRESH_RE = re.compile(
    r"^DISPLAY (?P<record>REFRESH|STATUS) "
    r"requested=(?P<requested>[a-z-]+) "
    r"applied=(?P<applied>[a-z-]+) "
    r"ok=(?P<ok>true|false) "
    r"duration_ms=(?P<duration_ms>\d+) "
    r"area=(?P<x>-?\d+),(?P<y>-?\d+),(?P<width>-?\d+),(?P<height>-?\d+) "
    r"sequence=(?P<sequence>\d+) "
    r"regional_debt=(?P<regional_debt>\d+)$"
)


@dataclass(frozen=True)
class RefreshReport:
    requested: str
    applied: str
    ok: bool
    duration_ms: int
    area: tuple[int, int, int, int]
    sequence: int
    regional_debt: int


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    connection.port = port
    connection.dtr = True
    connection.rts = False
    connection.open()
    return connection


def read_line(connection: serial.Serial, deadline: float) -> str | None:
    while time.monotonic() < deadline:
        raw_line = connection.readline()
        if not raw_line:
            continue
        line = raw_line.decode("utf-8", errors="replace").strip()
        if line:
            print(line, flush=True)
            return line
    return None


def parse_refresh(line: str) -> RefreshReport | None:
    match = REFRESH_RE.match(line)
    if match is None:
        return None
    values = match.groupdict()
    return RefreshReport(
        requested=values["requested"],
        applied=values["applied"],
        ok=values["ok"] == "true",
        duration_ms=int(values["duration_ms"]),
        area=(
            int(values["x"]),
            int(values["y"]),
            int(values["width"]),
            int(values["height"]),
        ),
        sequence=int(values["sequence"]),
        regional_debt=int(values["regional_debt"]),
    )


def wait_for_new_report(
    connection: serial.Serial, previous_sequence: int, timeout_seconds: float
) -> RefreshReport | None:
    deadline = time.monotonic() + timeout_seconds
    while True:
        line = read_line(connection, deadline)
        if line is None:
            return None
        report = parse_refresh(line)
        if report is not None and report.sequence > previous_sequence:
            return report


def send_and_wait_for_refresh(
    port: str, command: str, previous_sequence: int, timeout_seconds: float
) -> RefreshReport:
    print(f"> {command}", flush=True)
    with open_connection(port) as connection:
        time.sleep(0.15)
        connection.reset_input_buffer()
        connection.write(f"{command}\n".encode("ascii"))
        connection.flush()
        report = wait_for_new_report(
            connection, previous_sequence, timeout_seconds
        )
        if report is not None:
            return report

    # EPD output temporarily blocks native ESP32-S3 USB interrupts. Windows
    # occasionally keeps that CDC endpoint stalled even after the physical
    # refresh completes. Reopening without RTS does not reset the board; query
    # the result stored by firmware instead of misreporting a display failure.
    print("[USB CDC packet delayed; reconnecting for DISPLAY STATUS]", flush=True)
    with open_connection(port) as connection:
        time.sleep(0.15)
        connection.reset_input_buffer()
        connection.write(b"DISPLAY STATUS\n")
        connection.flush()
        report = wait_for_new_report(
            connection, previous_sequence, timeout_seconds
        )
        if report is not None:
            return report

    raise TimeoutError(f"No completed physical refresh after {command!r}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    print(
        "DISPLAY_REFRESH_TEST_RETIRED: firmware 0.3.2 was visually rejected; "
        "automatic rendering and partial refresh are quarantined.",
        file=sys.stderr,
    )
    return 2


def historical_main() -> int:
    """Retain the rejected machine-only experiment without running it."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--timeout", type=float, default=4.0)
    args = parser.parse_args()

    commands = [
        "INPUT UP SHORT",
        "INPUT DOWN SHORT",
        "INPUT CENTER SHORT",
        "INPUT UP LONG",
        "INPUT DOWN LONG",
        "INPUT CENTER LONG",
        "INPUT CENTER DOUBLE",
        "INPUT UP SHORT",
        "INPUT DOWN SHORT",
        "INPUT CENTER SHORT",
        "INPUT UP LONG",
        "INPUT DOWN LONG",
    ]

    try:
        sequence = 0
        recovery = send_and_wait_for_refresh(
            args.port, "DISPLAY RECOVER", sequence, args.timeout
        )
        sequence = recovery.sequence
        require(recovery.ok, "Recovery refresh failed")
        require(recovery.requested == "recovery-full", "Recovery was not requested")
        require(recovery.applied == "recovery-full", "Recovery was not applied")
        require(recovery.area == (0, 0, 960, 540), "Recovery did not cover screen")
        require(recovery.regional_debt == 0, "Recovery did not reset region debt")

        reports: list[RefreshReport] = []
        for command in commands:
            report = send_and_wait_for_refresh(
                args.port, command, sequence, args.timeout
            )
            sequence = report.sequence
            require(report.ok, f"Refresh failed after {command}")
            require(report.requested == "quality-region", "Input was not regional")
            reports.append(report)

        for index, report in enumerate(reports[:10], start=1):
            require(
                report.applied == "quality-region",
                f"Regional refresh {index} was unexpectedly promoted",
            )
            require(
                report.area == (496, 338, 432, 126),
                f"Regional refresh {index} used the wrong area",
            )
            require(
                report.regional_debt == index,
                f"Regional debt mismatch after refresh {index}",
            )

        promoted = reports[10]
        require(promoted.applied == "quality-full", "Budget did not force full refresh")
        require(promoted.area == (0, 0, 960, 540), "Promoted refresh was not full")
        require(promoted.regional_debt == 0, "Promoted refresh did not reset debt")

        after_promotion = reports[11]
        require(
            after_promotion.applied == "quality-region",
            "Regional refresh did not resume after full cleanup",
        )
        require(after_promotion.regional_debt == 1, "Debt did not restart at one")

        final_refresh = send_and_wait_for_refresh(
            args.port, "DIAG REFRESH", sequence, args.timeout
        )
        require(final_refresh.ok, "Final full refresh failed")
        require(final_refresh.applied == "quality-full", "Final refresh was not full")
        require(final_refresh.regional_debt == 0, "Final refresh did not reset debt")

    except (OSError, serial.SerialException, TimeoutError, AssertionError) as error:
        print(f"DISPLAY_REFRESH_TEST_FAILED: {error}", file=sys.stderr)
        return 1

    region_durations = [
        report.duration_ms
        for report in reports
        if report.applied == "quality-region"
    ]
    print(
        "DISPLAY_REFRESH_TEST_ACCEPTED "
        f"recovery_ms={recovery.duration_ms} "
        f"region_min_ms={min(region_durations)} "
        f"region_max_ms={max(region_durations)} "
        f"promoted_full_ms={promoted.duration_ms} "
        f"final_full_ms={final_refresh.duration_ms}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
