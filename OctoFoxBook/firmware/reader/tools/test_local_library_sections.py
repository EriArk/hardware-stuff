"""Exercise local sections/groups/search and hash rendered framebuffers."""

from __future__ import annotations

import argparse
import re
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


def read_until(
    connection: serial.Serial, marker: str, seconds: float = 24.0
) -> list[str]:
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


def transact(
    connection: serial.Serial,
    command: str,
    marker: str,
    seconds: float = 24.0,
) -> list[str]:
    connection.reset_input_buffer()
    connection.write((command + "\n").encode("ascii"))
    connection.flush()
    return read_until(connection, marker, seconds)


def burst(
    connection: serial.Serial, command: str, count: int, marker: str
) -> list[str]:
    connection.reset_input_buffer()
    connection.write(((command + "\n") * count).encode("ascii"))
    connection.flush()
    return read_until(connection, marker)


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z0-9_]+)=([^\s]+)", line))


def status(connection: serial.Serial) -> dict[str, str]:
    lines = transact(connection, "LIBRARY STATUS", "LIBRARY STATUS", 4.0)
    return fields(next(line for line in lines if line.startswith("LIBRARY STATUS")))


def snapshot(connection: serial.Serial) -> dict[str, str]:
    lines = transact(
        connection, "DISPLAY SNAPSHOT STATUS", "DISPLAY SNAPSHOT", 4.0
    )
    result = fields(
        next(line for line in lines if line.startswith("DISPLAY SNAPSHOT"))
    )
    if result.get("available") != "true":
        raise AssertionError(f"Framebuffer unavailable: {result}")
    pixels = sum(int(result[name]) for name in ("black", "gray", "white"))
    if result.get("width") != "540" or result.get("height") != "960":
        raise AssertionError(f"Unexpected snapshot dimensions: {result}")
    if pixels != 540 * 960 or int(result["black"]) == 0 or int(result["white"]) == 0:
        raise AssertionError(f"Invalid snapshot coverage: {result}")
    return result


def require_single_refresh(lines: list[str], label: str) -> None:
    # Count completed screen transactions, not cleanup scheduling/status logs.
    refreshes = [
        line for line in lines
        if line.startswith("DISPLAY ") and " requested=" in line and " applied=" in line
    ]
    if len(refreshes) != 1:
        raise AssertionError(
            f"{label}: expected one display transaction, got {len(refreshes)}"
        )


def require_state(
    current: dict[str, str], phase: int, section: int, minimum_rows: int
) -> None:
    if (
        current.get("active") != "true"
        or int(current.get("phase", "-1")) != phase
        or int(current.get("section", "-1")) != section
        or int(current.get("rows", "0")) < minimum_rows
    ):
        raise AssertionError(
            f"Unexpected local state; wanted phase={phase} section={section} "
            f"rows>={minimum_rows}, got {current}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()

    try:
        with open_connection(args.port) as connection:
            time.sleep(0.75)

            opened = transact(connection, "LIBRARY OPEN", "LIBRARY OPEN COMPLETE")
            require_single_refresh(opened, "sections-open")
            require_state(status(connection), phase=0, section=0, minimum_rows=7)
            root_snapshot = snapshot(connection)

            all_books = transact(
                connection, "INPUT CENTER SHORT", "LIBRARY OPEN COMPLETE"
            )
            require_single_refresh(all_books, "all-books-open")
            require_state(status(connection), phase=2, section=0, minimum_rows=1)
            all_snapshot = snapshot(connection)

            root_again = transact(
                connection, "INPUT CENTER LONG", "LIBRARY OPEN COMPLETE"
            )
            require_single_refresh(root_again, "all-books-back")
            require_state(status(connection), phase=0, section=0, minimum_rows=7)
            if snapshot(connection)["fnv1a"] != root_snapshot["fnv1a"]:
                raise AssertionError("Root framebuffer changed after a round trip")

            authors_selected = burst(
                connection, "INPUT DOWN SHORT", 4, "LIBRARY OPEN COMPLETE"
            )
            require_single_refresh(authors_selected, "authors-select")
            authors = transact(
                connection, "INPUT CENTER SHORT", "LIBRARY OPEN COMPLETE"
            )
            require_single_refresh(authors, "authors-open")
            require_state(status(connection), phase=1, section=4, minimum_rows=1)
            authors_snapshot = snapshot(connection)

            author_books = transact(
                connection, "INPUT CENTER SHORT", "LIBRARY OPEN COMPLETE"
            )
            require_single_refresh(author_books, "author-books-open")
            require_state(status(connection), phase=2, section=4, minimum_rows=1)

            transact(connection, "INPUT CENTER LONG", "LIBRARY OPEN COMPLETE")
            require_state(status(connection), phase=1, section=4, minimum_rows=1)
            transact(connection, "INPUT CENTER LONG", "LIBRARY OPEN COMPLETE")
            require_state(status(connection), phase=0, section=0, minimum_rows=7)

            search = transact(connection, "SEARCH OPEN", "SEARCH OPEN COMPLETE")
            require_single_refresh(search, "search-open")
            scope = burst(
                connection, "INPUT DOWN SHORT", 4, "SEARCH OPEN COMPLETE"
            )
            require_single_refresh(scope, "local-scope-select")
            transact(connection, "INPUT CENTER SHORT", "SEARCH OPEN COMPLETE")
            transact(connection, "INPUT CENTER SHORT", "SEARCH OPEN COMPLETE")
            results = transact(
                connection, "INPUT CENTER SHORT", "LIBRARY OPEN COMPLETE"
            )
            require_single_refresh(results, "offline-search-results")
            result_state = status(connection)
            require_state(result_state, phase=2, section=7, minimum_rows=0)
            if result_state.get("owner") != "SEARCH":
                raise AssertionError(f"Offline results lost Search ownership: {result_state}")
            result_snapshot = snapshot(connection)
            returned = transact(
                connection, "INPUT CENTER LONG", "SEARCH OPEN COMPLETE"
            )
            require_single_refresh(returned, "offline-search-back")

            hashes = {
                root_snapshot["fnv1a"],
                all_snapshot["fnv1a"],
                authors_snapshot["fnv1a"],
                result_snapshot["fnv1a"],
            }
            if len(hashes) != 4:
                raise AssertionError(f"Expected four distinct UI snapshots, got {hashes}")

    except (AssertionError, OSError, serial.SerialException, TimeoutError) as error:
        print(error, file=sys.stderr)
        return 1

    print(
        "LOCAL_LIBRARY_ACCEPTED sections=7 groups=authors offline_search=true "
        "snapshots=4 root_roundtrip=stable"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
