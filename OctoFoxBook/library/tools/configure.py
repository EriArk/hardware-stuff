"""Create local Compose settings without printing or overwriting passwords."""
import argparse
import ipaddress
import os
from pathlib import Path
import secrets
from urllib.parse import urlsplit


def configuration(origin, bind, port, admin_port):
    url = urlsplit(origin)
    if (url.scheme not in {"http", "https"} or not url.hostname or url.username
            or url.password or url.path not in {"", "/"} or url.query or url.fragment
            or any(c.isspace() or c in "#$'\"\\" for c in origin)):
        raise ValueError("Use an HTTP(S) origin, for example http://192.168.1.20:8080")
    url.port  # Validate malformed ports before creating a file.
    if not 1 <= port <= 65535 or not 1 <= admin_port <= 65535 or port == admin_port:
        raise ValueError("Choose distinct ports between 1 and 65535")
    if ipaddress.ip_address(bind).version != 4:
        raise ValueError("The initial Compose binding requires an IPv4 address")
    return {
        "OCTOFOX_ORIGIN": origin.rstrip("/"),
        "OCTOFOX_BIND_ADDRESS": bind,
        "OCTOFOX_PORT": str(port),
        "BOOKLORE_ADMIN_PORT": str(admin_port),
        "BOOKLORE_DB_PASSWORD": secrets.token_hex(32),
        "MARIADB_ROOT_PASSWORD": secrets.token_hex(32),
        "TZ": "UTC",
    }


def write_configuration(output, values):
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as out:
        out.write("".join(f"{key}={value}\n" for key, value in values.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", default="http://localhost:8080")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--admin-port", type=int, default=8081)
    parser.add_argument("--output", type=Path, default=Path(".env"))
    args = parser.parse_args()
    try:
        values = configuration(args.origin, args.bind, args.port, args.admin_port)
        write_configuration(args.output, values)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Configuration not written: {error}\n")
    print(f"Created {args.output}. Keep this file private and include it in your backup.")


if __name__ == "__main__":
    main()
