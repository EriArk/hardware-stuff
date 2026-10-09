"""Safely upload one complete FB2 to the reader's microSD over USB CDC."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import time
from pathlib import Path

import serial

from capture_board_diagnostics import open_connection


BOOK_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")


def read_line(
    connection: serial.Serial, deadline: float, *, echo: bool = True
) -> str | None:
    while time.monotonic() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line and echo:
            print(line, flush=True)
        if line:
            return line
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("--book-id", required=True)
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()

    if not BOOK_ID_RE.fullmatch(args.book_id):
        parser.error("--book-id must contain only letters, digits, '-' or '_'")
    if not args.path.is_file():
        parser.error(f"FB2 does not exist: {args.path}")

    size = args.path.stat().st_size
    digest = hashlib.sha256()
    with args.path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    sha256 = digest.hexdigest()

    command = f"BOOK PUT {args.book_id} {size} {sha256}\n"
    with open_connection(args.port) as connection:
        time.sleep(0.25)
        connection.reset_input_buffer()
        connection.write(command.encode("ascii"))
        connection.flush()

        deadline = time.monotonic() + 8.0
        ready = False
        while time.monotonic() < deadline:
            line = read_line(connection, deadline)
            if line is None:
                break
            if line.startswith("ERROR BOOK_PUT"):
                return 1
            if line == f"BOOK PUT READY id={args.book_id} bytes={size}":
                ready = True
                break
        if not ready:
            print("BOOK_UPLOAD_FAILED: device did not become ready", file=sys.stderr)
            return 2

        sent = 0
        next_report = 10
        with args.path.open("rb") as source:
            while chunk := source.read(256):
                connection.write(chunk)
                connection.flush()
                sent += len(chunk)
                percent = sent * 100 // size
                if percent >= next_report:
                    print(f"UPLOAD {percent}% ({sent:,}/{size:,})", flush=True)
                    next_report += 10

                if sent < size:
                    expected_progress = (
                        f"BOOK PUT PROGRESS id={args.book_id} received={sent}"
                    )
                    # FAT32 can occasionally pause for allocation/flush work.
                    # Keep the stop-and-wait window small, but allow the board
                    # enough time to acknowledge a block without abandoning a
                    # valid transfer during one of those SD-card stalls.
                    progress_deadline = time.monotonic() + 90.0
                    acknowledged = False
                    while time.monotonic() < progress_deadline:
                        line = read_line(
                            connection, progress_deadline, echo=False
                        )
                        if line is None:
                            break
                        if line == expected_progress:
                            acknowledged = True
                            break
                        if line.startswith("ERROR BOOK_PUT"):
                            return 1
                    if not acknowledged:
                        print(
                            f"BOOK_UPLOAD_FAILED: no acknowledgement at {sent}",
                            file=sys.stderr,
                        )
                        return 2

        deadline = time.monotonic() + 35.0
        expected = (
            f"BOOK PUT COMPLETE id={args.book_id} bytes={size} sha256={sha256}"
        )
        while time.monotonic() < deadline:
            line = read_line(connection, deadline)
            if line is None:
                break
            if line == expected:
                print("BOOK_UPLOAD_ACCEPTED", flush=True)
                return 0
            if line.startswith("ERROR BOOK_PUT"):
                return 1

    print("BOOK_UPLOAD_FAILED: no validated completion", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
