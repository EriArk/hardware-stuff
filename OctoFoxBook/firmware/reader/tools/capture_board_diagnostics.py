"""Capture and validate the diagnostics firmware report over USB serial."""

from __future__ import annotations

import argparse
import json
import sys
import time

import serial


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    connection.port = port
    # Native ESP32-S3 USB CDC needs DTR asserted for reliable continuous TX.
    # Keeping RTS low avoids the esptool reset sequence.
    connection.dtr = True
    connection.rts = False
    connection.open()
    return connection


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--seconds", type=float, default=12.0)
    args = parser.parse_args()

    report: dict[str, object] | None = None
    with open_connection(args.port) as connection:
        time.sleep(1.0)
        connection.reset_input_buffer()
        connection.write(b"PING\nDIAG STATUS\n")
        connection.flush()

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            raw_line = connection.readline()
            if not raw_line:
                continue
            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            print(line)
            if line.startswith("DIAG "):
                report = json.loads(line.removeprefix("DIAG "))
                break

    if report is None:
        print("No DIAG report received.", file=sys.stderr)
        return 2

    required_passes = {
        "psram_test": report.get("psram_test") is True,
        "sd_mounted": report.get("sd_mounted") is True,
        "sd_write": report.get("sd_write") is True,
        "rtc": report.get("rtc") is True,
        "battery_mv": 3000 <= int(report.get("battery_mv", 0)) <= 4500,
    }
    failed = [name for name, passed in required_passes.items() if not passed]
    if failed:
        print(f"Diagnostics failed: {', '.join(failed)}", file=sys.stderr)
        return 1

    print("DIAGNOSTICS_ACCEPTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
