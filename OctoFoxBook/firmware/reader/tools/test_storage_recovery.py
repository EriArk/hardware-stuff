#!/usr/bin/env python3
"""Physically prove boot recovery and malformed-FB2 containment on microSD."""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    connection.port = port
    connection.dtr = True
    connection.rts = False
    connection.open()
    return connection


def send(connection: serial.Serial, command: str) -> None:
    print(f"> {command}")
    connection.write((command + "\n").encode("ascii"))
    connection.flush()


def transact(
    connection: serial.Serial, command: str, marker: str, timeout: float = 8.0
) -> list[str]:
    connection.reset_input_buffer()
    send(connection, command)
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            continue
        print(line)
        lines.append(line)
        if line.startswith("ERROR "):
            raise RuntimeError(line)
        if marker in line:
            return lines
    raise TimeoutError(f"timed out waiting for {marker!r}")


def wait_for_restart(connection: serial.Serial, timeout: float = 20.0) -> list[str]:
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    next_ping = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if time.monotonic() >= next_ping:
            try:
                send(connection, "PING")
            except (OSError, serial.SerialException):
                pass
            next_ping = time.monotonic() + 1.0
        try:
            raw = connection.readline()
        except (OSError, serial.SerialException):
            time.sleep(0.25)
            continue
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            continue
        print(line)
        lines.append(line)
        if "PONG abyss-reader" in line:
            return lines
    raise TimeoutError("reader did not return after controlled restart")


def require(line: str, *tokens: str) -> None:
    missing = [token for token in tokens if token not in line]
    if missing:
        raise AssertionError(f"{line!r} is missing {missing}")


def numeric_field(line: str, name: str) -> int:
    match = re.search(rf"(?:^| ){re.escape(name)}=(\d+)(?: |$)", line)
    if match is None:
        raise AssertionError(f"{name!r} missing from {line!r}")
    return int(match.group(1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()

    try:
        with open_connection(args.port) as connection:
            time.sleep(0.75)
            pong = transact(connection, "PING", "PONG abyss-reader")[-1]
            require(pong, "PONG abyss-reader")

            armed = transact(
                connection,
                "STORAGE RECOVERY TEST ARM CONFIRM",
                "STORAGE RECOVERY TEST armed=",
            )[-1]
            require(armed, "armed=true")

            transact(
                connection,
                "SYSTEM RESTART CONFIRM",
                "SYSTEM RESTARTING",
            )
            boot_lines = wait_for_restart(connection)
            recovery = next(
                line
                for line in boot_lines
                if line.startswith("STORAGE RECOVERY source=boot")
            )
            require(recovery, "ran=true", "ok=true", "errors=0")
            if numeric_field(recovery, "restored") < 2:
                raise AssertionError("boot did not restore both old files")
            if numeric_field(recovery, "partials_removed") < 3:
                raise AssertionError("boot did not remove all partial files")
            if numeric_field(recovery, "rollbacks_removed") < 1:
                raise AssertionError("boot did not clean stale rollback copy")

            verified = transact(
                connection,
                "STORAGE RECOVERY TEST VERIFY",
                "STORAGE RECOVERY TEST verified=",
            )[-1]
            require(
                verified,
                "verified=true",
                "book=true",
                "state=true",
                "published=true",
                "leftovers_absent=true",
                "cleanup=true",
            )

            malformed = transact(
                connection,
                "STORAGE MALFORMED TEST CONFIRM",
                "STORAGE MALFORMED TEST ok=",
                # FAT directory cleanup on this 4 GB card can occasionally
                # take longer than the tiny parser input itself.  Keep this
                # above the worst physical observation so a successful late
                # result is not mistaken for a firmware hang.
                timeout=45.0,
            )[-1]
            require(
                malformed,
                "ok=true",
                "rejected=true",
                "original=true",
                "partials_absent=true",
                "cleanup=true",
            )

            status = transact(
                connection,
                "STORAGE RECOVERY STATUS",
                "STORAGE RECOVERY source=usb-status",
            )[-1]
            require(status, "ran=true", "ok=true", "errors=0")
    except (
        AssertionError,
        OSError,
        RuntimeError,
        StopIteration,
        serial.SerialException,
        TimeoutError,
    ) as error:
        print(f"STORAGE_RECOVERY_FAILED {error}", file=sys.stderr)
        return 1

    print(
        "STORAGE_RECOVERY_ACCEPTED atomic_restart=true malformed_rejected=true "
        "canonical_preserved=true leftovers=false"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
