"""Verify explicit preparation and warm reuse; idle preparation is disabled."""

from __future__ import annotations

import argparse
import re
import sys

import serial

from test_opds_download_offline import open_connection, require_field, transact


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--book-id", required=True, help="An existing local FB2 ID")
    parser.add_argument("--seconds", type=float, default=240.0)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", args.book_id):
        parser.error("--book-id must be a valid local book ID")
    try:
        with open_connection(args.port) as connection:
            for attempt in range(2):
                lines = transact(
                    connection, f"BOOK PREPARE {args.book_id}",
                    ("BOOK PREPARE COMPLETE", "ERROR "), args.seconds,
                )
                complete = lines[-1]
                if complete.startswith("ERROR "):
                    raise AssertionError(complete)
                require_field(complete, "id", args.book_id)
                if attempt == 1:
                    require_field(complete, "cache_reused", "true")
                    require_field(complete, "pagination_reused", "true")
            print(f"LIBRARY_PREPARATION_ACCEPTED id={args.book_id} warm_reuse=true")
            return 0
    except (AssertionError, OSError, serial.SerialException, TimeoutError) as error:
        print(f"LIBRARY_PREPARATION_FAILED {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
