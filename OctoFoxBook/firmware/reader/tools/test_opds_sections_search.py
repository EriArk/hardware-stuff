"""Accept all daily-use OPDS sections and keyboardless search on real hardware."""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")


def open_connection(port: str) -> serial.Serial:
    connection = serial.Serial(port=None, baudrate=115200, timeout=1.0)
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
    raise TimeoutError("Reader did not answer the 0.12.0 PING handshake")


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
        if line.startswith("ERROR CATALOG") or line.startswith("ERROR SEARCH"):
            raise AssertionError(line)
        if marker in line:
            return lines
    raise TimeoutError(f"Timeout waiting for {marker!r}; last={lines[-6:]}")


def require_single_refresh(lines: list[str], label: str) -> None:
    refreshes = [line for line in lines if line.startswith("DISPLAY ")
                 and " requested=" in line and " applied=" in line]
    if len(refreshes) != 1:
        raise AssertionError(
            f"{label}: expected one logical display transaction, got {len(refreshes)}"
        )


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^ ]+)", line))


def catalog_status(connection: serial.Serial) -> dict[str, str]:
    lines = transact(connection, "CATALOG STATUS", "radio=off", 5.0)
    return fields(next(line for line in lines if line.startswith("CATALOG STATUS")))


def search_status(connection: serial.Serial) -> dict[str, str]:
    lines = transact(connection, "SEARCH STATUS", "radio=off", 5.0)
    return fields(next(line for line in lines if line.startswith("SEARCH STATUS")))


def require(actual: dict[str, str], **expected: str) -> None:
    for name, wanted in expected.items():
        observed = actual.get(name)
        if observed != wanted:
            raise AssertionError(
                f"Expected {name}={wanted!r}, got {observed!r} in {actual!r}"
            )


def move(
    connection: serial.Serial,
    direction: str,
    count: int,
    marker: str,
    label: str,
) -> None:
    if direction not in {"UP", "DOWN"} or not 1 <= count <= 12:
        raise ValueError("Invalid coalesced move")
    lines = transact(
        connection,
        "\n".join(f"INPUT {direction} SHORT" for _ in range(count)),
        marker,
        20.0,
    )
    require_single_refresh(lines, label)


def open_root_entry(connection: serial.Serial, ordinal: int, label: str) -> None:
    move(connection, "DOWN", ordinal - 1, "CATALOG OPEN COMPLETE", f"{label}-select")
    lines = transact(
        connection, "INPUT CENTER SHORT", "CATALOG OPEN COMPLETE", 45.0
    )
    require_single_refresh(lines, f"{label}-open")
    status = catalog_status(connection)
    require(status, loaded="true", active="true", owner="CATALOG", radio="off")
    if int(status.get("entries", "0")) < 1:
        raise AssertionError(f"{label} returned no entries: {status}")


def back_to_root(connection: serial.Serial, levels: int, label: str) -> None:
    for level in range(levels):
        lines = transact(
            connection, "INPUT CENTER LONG", "CATALOG OPEN COMPLETE", 45.0
        )
        require_single_refresh(lines, f"{label}-back-{level + 1}")
    require(
        catalog_status(connection),
        entries="7",
        rows="7",
        selected="1",
        history="0",
        owner="CATALOG",
        radio="off",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()

    try:
        with open_connection(args.port) as connection:
            root_lines = transact(connection, "CATALOG OPEN", "CATALOG OPEN COMPLETE")
            require_single_refresh(root_lines, "root")
            require(
                catalog_status(connection),
                entries="7",
                rows="7",
                selected="1",
                history="0",
                owner="CATALOG",
                radio="off",
            )

            open_root_entry(connection, 3, "authors")
            back_to_root(connection, 1, "authors")

            open_root_entry(connection, 4, "series")
            back_to_root(connection, 1, "series")

            open_root_entry(connection, 5, "genres")
            for label in ("genre-group", "subgenre"):
                lines = transact(
                    connection,
                    "INPUT CENTER SHORT",
                    "CATALOG OPEN COMPLETE",
                    45.0,
                )
                require_single_refresh(lines, label)
                nested = catalog_status(connection)
                if int(nested.get("entries", "0")) < 1:
                    raise AssertionError(f"{label} returned no entries: {nested}")
            back_to_root(connection, 3, "genres")

            open_root_entry(connection, 6, "tags")
            tag_lines = transact(
                connection, "INPUT CENTER SHORT", "CATALOG OPEN COMPLETE", 45.0
            )
            require_single_refresh(tag_lines, "tag-books")
            if int(catalog_status(connection).get("entries", "0")) < 1:
                raise AssertionError("First tag returned no books")
            back_to_root(connection, 2, "tags")

            open_root_entry(connection, 7, "surprise")
            require(
                catalog_status(connection),
                entries="25",
                rows="25",
                next="false",
                previous="false",
                truncated="false",
                radio="off",
            )
            back_to_root(connection, 1, "surprise")

            search_lines = transact(connection, "SEARCH OPEN", "SEARCH OPEN COMPLETE")
            require_single_refresh(search_lines, "search-open")
            require(search_status(connection), active="true", phase="0", selected="1/4")

            move(connection, "DOWN", 2, "SEARCH OPEN COMPLETE", "search-authors")
            for label in ("search-scope", "search-range"):
                lines = transact(
                    connection, "INPUT CENTER SHORT", "SEARCH OPEN COMPLETE", 20.0
                )
                require_single_refresh(lines, label)
            results = transact(
                connection, "INPUT CENTER SHORT", "CATALOG OPEN COMPLETE", 45.0
            )
            require_single_refresh(results, "search-results")
            require(
                catalog_status(connection),
                loaded="true",
                active="true",
                owner="SEARCH",
                history="0",
                radio="off",
            )
            returned = transact(
                connection, "INPUT CENTER LONG", "SEARCH OPEN COMPLETE", 20.0
            )
            require_single_refresh(returned, "search-results-back")
            require(search_status(connection), active="true", phase="2")

        print(
            "OPDS_SECTIONS_SEARCH_ACCEPTED "
            "authors=true series=true genres=true subgenres=true tags=true "
            "surprise=25 search=three-button tls=verified radio=off"
        )
        return 0
    except (AssertionError, OSError, serial.SerialException, TimeoutError) as error:
        print(f"OPDS_SECTIONS_SEARCH_FAILED {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
