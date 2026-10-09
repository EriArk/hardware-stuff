"""Verify persisted provisioning and a strict authenticated OPDS root request."""

from __future__ import annotations

import argparse
import sys
import time

import serial


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    connection.port = port
    connection.dtr = True
    connection.rts = False
    connection.open()
    return connection


def transact(
    connection: serial.Serial, command: str, marker: str, seconds: float = 12.0
) -> list[str]:
    connection.reset_input_buffer()
    connection.write((command + "\n").encode("ascii"))
    connection.flush()
    lines: list[str] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        raw_line = connection.readline()
        if not raw_line:
            continue
        line = raw_line.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        print(line)
        if marker in line:
            return lines
    raise TimeoutError(f"Timed out waiting for {marker!r}")


def require(line: str, *tokens: str) -> None:
    missing = [token for token in tokens if token not in line]
    if missing:
        raise AssertionError(f"Missing acceptance fields: {', '.join(missing)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()

    try:
        with open_connection(args.port) as connection:
            time.sleep(0.75)
            provision = transact(
                connection, "PROVISION STATUS", "PROVISION STATUS"
            )[-1]
            require(provision, "configured=true", "fields=complete")

            before = transact(connection, "NETWORK STATUS", "NETWORK STATUS")[-1]
            require(before, "configured=true", "connected=false", "radio=off")

            probe_lines = transact(
                connection, "OPDS TEST", "OPDS TEST COMPLETE", seconds=65.0
            )
            probe = next(
                line for line in probe_lines if "OPDS TEST COMPLETE" in line
            )
            require(
                probe,
                "ok=true",
                "http=200",
                "tls=verified",
                "tls_error=0",
                "atom=true",
                "radio=off",
            )

            after = transact(connection, "NETWORK STATUS", "NETWORK STATUS")[-1]
            require(after, "configured=true", "connected=false", "radio=off")
    except (AssertionError, OSError, serial.SerialException, TimeoutError) as error:
        print(error, file=sys.stderr)
        return 1

    print("OPDS_CONNECTION_ACCEPTED auth=stored tls=verified atom=true radio=off")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
