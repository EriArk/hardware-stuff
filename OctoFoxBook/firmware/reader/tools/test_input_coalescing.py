"""Verify that burst navigation produces one display refresh and one jump."""

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
    return connection


def read_until(connection: serial.Serial, marker: str, seconds: float) -> list[str]:
    lines: list[str] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        raw_line = connection.readline()
        if not raw_line:
            continue
        line = raw_line.decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
            print(line)
        if marker in line:
            return lines
    raise TimeoutError(f"Timed out waiting for {marker!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()

    try:
        with open_connection(args.port) as connection:
            time.sleep(0.75)
            connection.reset_input_buffer()
            connection.write(b"LIBRARY OPEN\n")
            connection.flush()
            read_until(connection, "LIBRARY OPEN COMPLETE", 15.0)

            # The local library opens on the seven-section hub. Enter
            # "All books" before exercising burst navigation inside the list.
            connection.reset_input_buffer()
            connection.write(b"INPUT CENTER SHORT\n")
            connection.flush()
            read_until(connection, "LIBRARY OPEN COMPLETE", 15.0)

            connection.reset_input_buffer()
            connection.write(b"INPUT DOWN SHORT\n" * 3)
            connection.flush()
            lines = read_until(connection, "LIBRARY OPEN COMPLETE", 15.0)
    except (OSError, serial.SerialException, TimeoutError) as error:
        print(error, file=sys.stderr)
        return 2

    ready = [line for line in lines if line.startswith("LIBRARY READY ")]
    refreshes = [line for line in lines if line.startswith("DISPLAY LIBRARY ")]
    events = [line for line in lines if line.startswith("EVENT DOWN SHORT ")]
    if len(events) != 3 or len(ready) != 1 or len(refreshes) != 1:
        print(
            f"Unexpected counts: events={len(events)} ready={len(ready)} "
            f"refreshes={len(refreshes)}",
            file=sys.stderr,
        )
        return 1
    selected = re.search(r"selected=(\d+)", ready[0])
    if selected is None or int(selected.group(1)) != 4:
        print(f"Burst did not jump to row 4: {ready[0]}", file=sys.stderr)
        return 1
    print("INPUT_COALESCING_ACCEPTED events=3 refreshes=1 selected=4")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
