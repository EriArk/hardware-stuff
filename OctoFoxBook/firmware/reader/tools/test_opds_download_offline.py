"""Accept authenticated OPDS download, atomic storage, and offline reuse."""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.5)
    connection.port = port
    connection.dtr = True
    connection.rts = False
    connection.open()
    wait_for_ping(connection, 12.0)
    connection.reset_input_buffer()
    return connection


def wait_for_ping(connection: serial.Serial, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        connection.reset_input_buffer()
        connection.write(b"PING\n")
        connection.flush()
        attempt = time.monotonic() + 1.0
        while time.monotonic() < attempt:
            line = read_line(connection)
            if "PONG abyss-reader" in line:
                return
        time.sleep(0.2)
    raise TimeoutError("Reader did not answer the PING handshake")


def read_line(connection: serial.Serial) -> str:
    raw = connection.readline()
    if not raw:
        return ""
    return raw.decode("utf-8", errors="replace").strip()


def transact(
    connection: serial.Serial,
    command: str,
    markers: tuple[str, ...],
    seconds: float = 45.0,
) -> list[str]:
    connection.reset_input_buffer()
    connection.write((command + "\n").encode("ascii"))
    connection.flush()
    lines: list[str] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = read_line(connection)
        if not line:
            continue
        lines.append(line)
        print(line)
        if any(marker in line for marker in markers):
            return lines
    raise TimeoutError(
        f"Timeout waiting for {markers!r}; last={lines[-10:]}"
    )


def require_field(line: str, name: str, expected: str) -> None:
    match = re.search(rf"(?:^| ){re.escape(name)}=([^ ]+)", line)
    actual = match.group(1) if match else None
    if actual != expected:
        raise AssertionError(
            f"Expected {name}={expected!r}, got {actual!r} in {line!r}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    parser.add_argument(
        "--entry",
        type=int,
        default=8,
        help="One-based item in the newest feed that is not already local",
    )
    args = parser.parse_args()

    if args.entry < 1:
        parser.error("--entry must be at least 1")

    try:
        with open_connection(args.port) as connection:
            # Pick an item outside the initial seven-book fixture so this
            # exercises a real network acquisition instead of a local hit.
            transact(connection, "CATALOG OPEN", ("CATALOG OPEN COMPLETE",))
            transact(
                connection,
                "INPUT CENTER SHORT",
                ("CATALOG OPEN COMPLETE",),
            )
            if args.entry > 1:
                transact(
                    connection,
                    "\n".join("INPUT DOWN SHORT" for _ in range(args.entry - 1)),
                    ("CATALOG OPEN COMPLETE",),
                    25.0,
                )
            card = transact(connection, "INPUT CENTER SHORT", ("BOOK CARD OPEN",))
            if any("DOWNLOAD START" in line or "READER OPEN COMPLETE" in line for line in card):
                raise AssertionError("Selecting a book must only open its card")
            require_field(card[-1], "local_copy", "false")
            download = transact(
                connection,
                "INPUT CENTER SHORT",
                ("reason=download-complete", "reason=download-failed"),
                240.0,
            )
            failure = next(
                (line for line in download if line.startswith("DOWNLOAD FAILED")),
                None,
            )
            if failure:
                raise AssertionError(failure)
            if any("READER OPEN COMPLETE" in line for line in download):
                raise AssertionError("Download must stay on the card, not open the reader")
            require_field(download[-1], "local_copy", "true")
            require_field(download[-1], "downloading", "false")
            complete = next(
                line
                for line in download
                if line.startswith("DOWNLOAD COMPLETE")
            )
            match = re.search(r" id=([^ ]+)", complete)
            if match is None:
                raise AssertionError(f"Download ID missing: {complete}")
            book_id = match.group(1)
            require_field(complete, "tls", "true")
            require_field(complete, "radio", "off")
            require_field(complete, "atomic", "true")

            queue = transact(
                connection, "DOWNLOAD STATUS", ("DOWNLOAD STATUS",), 5.0
            )[-1]
            require_field(queue, "jobs", "0")
            require_field(queue, "radio", "off")

            stored = transact(
                connection,
                f"BOOK STATUS {book_id}",
                ("BOOK STATUS",),
                5.0,
            )[-1]
            require_field(stored, "present", "true")
            require_field(stored, "part", "false")
            require_field(stored, "old", "false")

            radio = transact(
                connection, "NETWORK STATUS", ("NETWORK STATUS",), 5.0
            )[-1]
            require_field(radio, "connected", "false")
            require_field(radio, "radio", "off")

            opened = transact(
                connection, "INPUT CENTER SHORT", ("READER OPEN COMPLETE",), 240.0
            )
            if not any("preparing=true" in line for line in opened):
                raise AssertionError("Read must show preparation on the card")
            page = transact(
                connection,
                "INPUT DOWN SHORT",
                ("READER PAGE COMPLETE",),
                30.0,
            )
            if any("NETWORK CONNECT" in line for line in page):
                raise AssertionError("Page turn unexpectedly used the network")

            transact(
                connection,
                "SYSTEM RESTART CONFIRM",
                ("SYSTEM RESTART",),
                5.0,
            )
            time.sleep(2.0)
            wait_for_ping(connection, 15.0)
            reopened = transact(
                connection,
                f"READER OPEN {book_id}",
                ("READER OPEN COMPLETE",),
                35.0,
            )
            if any("DOWNLOAD START" in line or "NETWORK CONNECT" in line for line in reopened):
                raise AssertionError("Warm open unexpectedly used the network")
            cache = next(
                line for line in reopened if line.startswith("READER CACHE ")
            )
            pagination = next(
                line
                for line in reopened
                if line.startswith("READER PAGINATION ")
            )
            require_field(cache, "reused", "true")
            require_field(pagination, "reused", "true")

            radio = transact(
                connection, "NETWORK STATUS", ("NETWORK STATUS",), 5.0
            )[-1]
            require_field(radio, "radio", "off")
            print(
                "OPDS_DOWNLOAD_OFFLINE_ACCEPTED "
                f"id={book_id} tls=verified atomic=true queue=empty "
                "page_turn=offline warm_open=offline radio=off"
            )
            return 0
    except (
        AssertionError,
        OSError,
        StopIteration,
        serial.SerialException,
        TimeoutError,
    ) as error:
        print(f"OPDS_DOWNLOAD_OFFLINE_FAILED {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
