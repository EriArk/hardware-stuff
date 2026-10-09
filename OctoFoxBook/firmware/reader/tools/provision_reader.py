"""Provision the reader's fixed Wi-Fi and OPDS account without logging secrets."""

from __future__ import annotations

import argparse
import base64
import os
import sys
import time
from pathlib import Path

import serial

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")


ENV_TO_FIELD = (
    ("ABYSS_WIFI_SSID", "SSID"),
    ("ABYSS_WIFI_PASSWORD", "WIFI_PASSWORD"),
    ("ABYSS_OPDS_URL", "OPDS_URL"),
    ("ABYSS_OPDS_USERNAME", "OPDS_USERNAME"),
    ("ABYSS_OPDS_PASSWORD", "OPDS_PASSWORD"),
)


def parse_env_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        return values
    for number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise ValueError(f"Invalid environment-file syntax at line {number}")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key] = value
    return values


def load_configuration(path: Path) -> dict[str, str]:
    values = parse_env_file(path)
    for env_name, _ in ENV_TO_FIELD:
        if env_name in os.environ:
            values[env_name] = os.environ[env_name]
    missing = [name for name, _ in ENV_TO_FIELD if not values.get(name)]
    if missing:
        raise ValueError("Missing required settings: " + ", ".join(missing))
    if not values["ABYSS_OPDS_URL"].startswith("https://"):
        raise ValueError("ABYSS_OPDS_URL must use HTTPS")
    return values


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    connection.port = port
    connection.dtr = True
    connection.rts = False
    connection.open()
    return connection


def open_with_retry(port: str, seconds: float = 25.0) -> serial.Serial:
    deadline = time.monotonic() + seconds
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            return open_connection(port)
        except (OSError, serial.SerialException) as error:
            last_error = error
            time.sleep(0.5)
    raise serial.SerialException(
        f"Reader serial port did not return within {seconds:.0f} seconds"
    ) from last_error


def transact(
    connection: serial.Serial,
    command: str,
    expected: str,
    label: str,
    seconds: float = 10.0,
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
        if line.startswith("ERROR "):
            raise RuntimeError(f"Reader rejected {label}")
        if expected in line:
            return lines
    raise TimeoutError(f"Timed out while waiting for {label}")


def encoded(value: str) -> str:
    return base64.b64encode(value.encode("utf-8")).decode("ascii")


def provision(connection: serial.Serial, values: dict[str, str]) -> None:
    transact(connection, "PROVISION BEGIN", "OK PROVISION BEGIN", "begin")
    for env_name, field_name in ENV_TO_FIELD:
        payload = encoded(values[env_name])
        transact(
            connection,
            f"PROVISION FIELD {field_name} {payload}",
            f"OK PROVISION FIELD name={field_name} accepted=true",
            f"field {field_name}",
        )
        payload = ""
    transact(
        connection,
        "PROVISION COMMIT",
        "PROVISION COMMIT COMPLETE configured=true",
        "atomic commit",
    )


def test_opds(connection: serial.Serial) -> None:
    lines = transact(
        connection,
        "OPDS TEST",
        "OPDS TEST COMPLETE",
        "authenticated OPDS test",
        seconds=65.0,
    )
    result = next(line for line in lines if "OPDS TEST COMPLETE" in line)
    required = ("ok=true", "http=200", "tls=verified", "atom=true", "radio=off")
    if not all(item in result for item in required):
        raise RuntimeError("Authenticated OPDS test did not meet acceptance criteria")


def resume_sync(connection: serial.Serial) -> None:
    # Clear the diagnostic pause, including on alpha21. Does not start Wi-Fi.
    transact(connection, "SYNC RESUME", "SYNC RESUMED", "release diagnostic pause")


def verify_after_restart(port: str, run_opds_test: bool) -> None:
    with open_with_retry(port) as connection:
        time.sleep(1.0)
        lines = transact(
            connection,
            "PROVISION STATUS",
            "PROVISION STATUS",
            "post-restart provisioning status",
        )
        status = next(line for line in lines if line.startswith("PROVISION STATUS"))
        if "configured=true" not in status or "fields=complete" not in status:
            raise RuntimeError("Provisioning did not survive restart")
        if run_opds_test:
            test_opds(connection)
        resume_sync(connection)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    parser.add_argument(
        "--env-file",
        type=Path,
        default=Path(__file__).resolve().parents[1] / ".env",
    )
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--restart-test", action="store_true")
    parser.add_argument("--check-only", action="store_true",
                        help="Validate the local profile without opening a serial port")
    args = parser.parse_args()

    values: dict[str, str] = {}
    try:
        values = load_configuration(args.env_file)
        if args.check_only:
            print("PROVISIONING_PROFILE_VALID secrets_logged=false device_changed=false")
            return 0
        with open_with_retry(args.port) as connection:
            time.sleep(0.75)
            provision(connection, values)
            if args.test:
                test_opds(connection)
            if args.restart_test:
                transact(
                    connection,
                    "SYSTEM RESTART CONFIRM",
                    "SYSTEM RESTARTING",
                    "controlled restart",
                )
            else:
                resume_sync(connection)
        if args.restart_test:
            time.sleep(2.0)
            verify_after_restart(args.port, args.test)
    except (
        OSError,
        RuntimeError,
        TimeoutError,
        ValueError,
        serial.SerialException,
    ) as error:
        print(f"Provisioning failed: {error}", file=sys.stderr)
        return 1
    finally:
        for env_name, _ in ENV_TO_FIELD:
            if env_name in values:
                values[env_name] = ""

    print(
        "PROVISIONING_ACCEPTED stored=nvs secrets_logged=false "
        f"opds_tested={str(args.test).lower()} restart_tested={str(args.restart_test).lower()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
