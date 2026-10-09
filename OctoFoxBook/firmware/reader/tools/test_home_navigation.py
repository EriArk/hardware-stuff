"""Exercise Home, reading and tab focus using USB input events on real hardware."""

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


def transact(
    connection: serial.Serial, command: str, marker: str, seconds: float = 20.0
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
        if line:
            lines.append(line)
            print(line)
        if marker in line:
            return lines
    raise TimeoutError(f"Timed out waiting for {marker!r} after {command!r}")


def require_single_refresh(lines: list[str], label: str) -> None:
    refreshes = [line for line in lines if line.startswith("DISPLAY ")
                 and " requested=" in line and " applied=" in line]
    if len(refreshes) != 1:
        raise AssertionError(
            f"{label}: expected one display transaction, got {len(refreshes)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()
    from provision_reader import resume_sync
    with open_connection(args.port) as connection:
        def step(command, marker):
            return transact(connection, command, marker, seconds=35)
        try:
            step("HOME OPEN", "HOME OPEN COMPLETE")
            status = step("HOME STATUS", "HOME STATUS")[-1]
            selected = int(re.search(r"selected=(\d+)", status)[1])
            # Select Continue without opening any other book or altering a page.
            for _ in range(abs(selected - 1)):
                step("INPUT UP SHORT", "HOME OPEN COMPLETE")
            assert "action=continue" in step("HOME STATUS", "HOME STATUS")[-1], "A previously opened book is required"
            opened = step("INPUT CENTER SHORT", "READER OPEN COMPLETE")
            assert "resumed=true" in opened[-1] and "recent_saved=true" in opened[-1]
            before = step("READER STATUS", "READER STATUS")[-1]
            step("INPUT CENTER SHORT", "READER MENU READY")
            assert "screen=12" in step("TAB STATUS", "TAB STATUS")[-1]
            step("INPUT CENTER DOUBLE", "HOME OPEN COMPLETE")
            step("INPUT CENTER LONG", "SECTIONS OPEN COMPLETE")
            step("INPUT DOWN SHORT", "SECTIONS OPEN COMPLETE")
            step("INPUT CENTER SHORT", "LIBRARY OPEN COMPLETE")
            step("INPUT DOWN SHORT", "LIBRARY OPEN COMPLETE")
            library_before = step("LIBRARY STATUS", "LIBRARY STATUS")[-1]
            step("INPUT CENTER DOUBLE", "HOME OPEN COMPLETE")
            step("INPUT CENTER LONG", "SECTIONS OPEN COMPLETE")
            step("INPUT DOWN SHORT", "SECTIONS OPEN COMPLETE")
            step("INPUT CENTER SHORT", "LIBRARY OPEN COMPLETE")
            library_after = step("LIBRARY STATUS", "LIBRARY STATUS")[-1]
            for field in ("phase", "selected"):
                assert re.search(field+r"=(\d+)", library_before)[1] == re.search(field+r"=(\d+)", library_after)[1]
            step("INPUT CENTER DOUBLE", "HOME OPEN COMPLETE")
            step("INPUT CENTER SHORT", "READER OPEN COMPLETE")
            after = step("READER STATUS", "READER STATUS")[-1]
            assert re.search(r"page=(\d+/\d+)", before)[1] == re.search(r"page=(\d+/\d+)", after)[1]
            step("INPUT CENTER DOUBLE", "HOME OPEN COMPLETE")
            # Double OK also wins while focus is on the tabs.
            step("INPUT CENTER LONG", "SECTIONS OPEN COMPLETE")
            step("INPUT CENTER DOUBLE", "HOME OPEN COMPLETE")
            # Reaching the header by UP must acquire actual tab focus.
            step("INPUT UP SHORT", "SECTIONS OPEN COMPLETE selected=1")
            step("INPUT UP SHORT", "SECTIONS OPEN COMPLETE selected=4")
            step("INPUT DOWN SHORT", "SECTIONS OPEN COMPLETE selected=1")
            for index, (tab, marker) in enumerate([
                ("HOME", "HOME OPEN COMPLETE"), ("ON_DEVICE", "LIBRARY OPEN COMPLETE"),
                ("SEARCH", "SEARCH OPEN COMPLETE"), ("FAVORITES", "FAVORITES OPEN")]):
                step("INPUT CENTER SHORT", marker)
                assert f"active={tab}" in step("TAB STATUS", "TAB STATUS")[-1]
                step("INPUT CENTER LONG", f"SECTIONS OPEN COMPLETE selected={index+1}")
                step("INPUT DOWN SHORT", f"SECTIONS OPEN COMPLETE selected={(index+1)%4+1}")
            step("INPUT CENTER SHORT", "HOME OPEN COMPLETE")
        finally:
            resume_sync(connection)
    print("HOME_NAVIGATION_ACCEPTED reading_menu=true tab_focus=true selection_retained=true page_retained=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
