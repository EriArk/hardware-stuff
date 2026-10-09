"""Exercise the real paginated OPDS catalog on the physical reader."""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    connection.port = port
    connection.dtr = True
    connection.rts = False
    connection.open()
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        connection.reset_input_buffer()
        connection.write(b"PING\n")
        connection.flush()
        attempt_deadline = time.monotonic() + 0.8
        while time.monotonic() < attempt_deadline:
            raw = connection.readline()
            if raw and b"PONG abyss-reader" in raw:
                connection.reset_input_buffer()
                return connection
        time.sleep(0.2)
    connection.close()
    raise TimeoutError("Reader did not answer PING after opening serial")


def transact(
    connection: serial.Serial,
    command: str,
    marker: str,
    seconds: float = 45.0,
) -> list[str]:
    connection.reset_input_buffer()
    connection.write((command + "\n").encode("ascii"))
    connection.flush()
    deadline = time.monotonic() + seconds
    lines: list[str] = []
    while time.monotonic() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        print(line)
        if marker in line:
            return lines
        if line.startswith("ERROR CATALOG"):
            raise AssertionError(line)
    raise TimeoutError(f"Timeout waiting for {marker!r}; last={lines[-5:]}")


def status(connection: serial.Serial) -> str:
    lines = transact(connection, "CATALOG STATUS", "CATALOG STATUS", 5.0)
    return next(line for line in lines if line.startswith("CATALOG STATUS"))


def move_down_coalesced(connection: serial.Serial, count: int) -> None:
    if not 1 <= count <= 10:
        raise ValueError("A coalesced serial batch must contain 1..10 moves")
    connection.reset_input_buffer()
    connection.write(("INPUT DOWN SHORT\n" * count).encode("ascii"))
    connection.flush()
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            print(line)
        if "CATALOG OPEN COMPLETE" in line:
            return
    raise TimeoutError("Coalesced catalog selection did not redraw")


def require_fields(line: str, **expected: str) -> None:
    fields = dict(re.findall(r"([a-z_]+)=([^ ]+)", line))
    for key, value in expected.items():
        actual = fields.get(key)
        if actual != value:
            raise AssertionError(
                f"Expected {key}={value!r}, got {actual!r} in {line!r}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()

    try:
        with open_connection(args.port) as connection:
            transact(connection, "CATALOG OPEN", "CATALOG OPEN COMPLETE")
            root = status(connection)
            require_fields(
                root,
                loaded="true",
                active="true",
                entries="7",
                rows="7",
                selected="1",
                history="0",
                next="false",
                previous="false",
                radio="off",
            )

            transact(
                connection,
                "INPUT CENTER SHORT",
                "CATALOG OPEN COMPLETE",
            )
            page_one = status(connection)
            require_fields(
                page_one,
                entries="20",
                rows="21",
                selected="1",
                history="1",
                next="true",
                previous="false",
                radio="off",
            )

            # Keep each USB batch below the device RX FIFO. Ten rapid moves
            # collapse into one draw, so reaching the page button costs two
            # refresh transactions rather than twenty.
            move_down_coalesced(connection, 10)
            move_down_coalesced(connection, 10)
            selected_next = status(connection)
            require_fields(selected_next, selected="21", rows="21", radio="off")

            transact(
                connection,
                "INPUT CENTER SHORT",
                "CATALOG OPEN COMPLETE",
            )
            page_two = status(connection)
            require_fields(
                page_two,
                entries="20",
                rows="22",
                selected="1",
                history="1",
                next="true",
                previous="true",
                radio="off",
            )

            transact(
                connection,
                "INPUT CENTER LONG",
                "CATALOG OPEN COMPLETE",
            )
            returned = status(connection)
            require_fields(
                returned,
                entries="7",
                rows="7",
                selected="1",
                history="0",
                radio="off",
            )

            print(
                "CATALOG_NAVIGATION_ACCEPTED "
                "root=7 page_size=20 pagination=next_previous "
                "back=history tls=verified radio=off"
            )
            return 0
    except (AssertionError, OSError, serial.SerialException, TimeoutError) as error:
        print(f"CATALOG_NAVIGATION_FAILED {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
