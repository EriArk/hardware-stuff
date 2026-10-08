"""Exercise the extracted application over HTTP with an isolated OPDS upstream."""
import base64
from http.cookiejar import CookieJar
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import build_opener, HTTPCookieProcessor, Request

from octofox_library.books_web import LibraryWeb, WebServer
from test_web_device_contents import PAYLOAD


class TestUpstream(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        allowed = {"Basic " + base64.b64encode(f"{user}:test-only".encode()).decode()
                   for user in ("alice", "bob")}
        status = 200 if self.headers.get("Authorization") in allowed else 401
        data = b'<feed xmlns="http://www.w3.org/2005/Atom" />'
        self.send_response(status)
        self.send_header("Content-Type", "application/atom+xml")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        upstream = ThreadingHTTPServer(("127.0.0.1", 0), TestUpstream)
        self.start(upstream)
        self.app = LibraryWeb(Path(self.temp.name) / "library.sqlite3",
                              f"http://127.0.0.1:{upstream.server_port}/api/v1/opds",
                              "http://localhost")
        web = WebServer(("127.0.0.1", 0), self.app)
        self.base = f"http://127.0.0.1:{web.server_port}"
        self.app.origin = self.base
        self.start(web)

    def start(self, server):
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

    def login(self, user):
        client = build_opener(HTTPCookieProcessor(CookieJar()))
        with client.open(Request(self.base + "/reader-api/login",
                                 json.dumps({"username": user, "password": "test-only"}).encode(),
                                 {"Content-Type": "application/json", "Origin": self.base}), timeout=5) as response:
            identity = json.load(response)
        self.assertEqual(set(identity), {"username", "csrf"})
        return client, {"X-CSRF-Token": identity["csrf"], "Origin": self.base}

    def test_login_upload_read_resume_and_account_isolation(self):
        client, headers = self.login("alice")
        with client.open(Request(self.base + "/reader-api/uploads", PAYLOAD,
                                 headers | {"Content-Type": "application/octet-stream",
                                            "X-Upload-Filename": "sample.fb2"}), timeout=5) as response:
            book = json.load(response)
        book_id = book["id"]
        with client.open(self.base + f"/reader-api/books/{book_id}/read", timeout=5) as response:
            self.assertIn("<p>", json.load(response)["html"])
        saved = {"chapter": 0, "offset": 0.25, "anchor": {"block": 0, "char": 3}}
        with client.open(Request(self.base + f"/reader-api/books/{book_id}/state", json.dumps(saved).encode(),
                                 headers | {"Content-Type": "application/json"}), timeout=5) as response:
            self.assertEqual(response.status, 200)
        # A new login still reads the account's server-side position.
        client, _ = self.login("alice")
        with client.open(self.base + f"/reader-api/books/{book_id}", timeout=5) as response:
            self.assertEqual(json.load(response)["reading"]["anchor"], saved["anchor"])
        other, _ = self.login("bob")
        for suffix in (f"/books/{book_id}", f"/uploads/{book_id}/download"):
            with self.assertRaises(HTTPError) as error:
                other.open(self.base + "/reader-api" + suffix, timeout=5)
            self.assertEqual(error.exception.code, 404)

    def test_removed_source_features_have_no_routes_or_assets(self):
        client, _ = self.login("alice")
        for path in ("/reader-api/flibusta-admin", "/reader-api/discovery/imports",
                     "/discovery.js", "/daemon-admin.js"):
            with self.assertRaises(HTTPError) as error:
                client.open(self.base + path, timeout=5)
            self.assertEqual(error.exception.code, 404)
        with client.open(self.base + "/", timeout=5) as response:
            page = response.read().decode()
        self.assertIn("OctoFox Library", page)
        self.assertNotIn("discoveryScreen", page)


class ConfigurationTests(unittest.TestCase):
    def test_generated_secrets_are_distinct_private_and_never_overwritten(self):
        tool = Path(__file__).parents[1] / "tools/configure.py"
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / ".env"
            command = [sys.executable, str(tool), "--output", str(output)]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            original = output.read_bytes()
            values = dict(line.split("=", 1) for line in original.decode().splitlines())
            password = values["BOOKLORE_DB_PASSWORD"]
            self.assertEqual(len(password), 64)
            self.assertNotEqual(password, values["MARIADB_ROOT_PASSWORD"])
            self.assertNotIn(password, first.stdout + first.stderr)
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(output.read_bytes(), original)

    def test_malformed_origins_cannot_inject_environment_settings(self):
        tool = Path(__file__).parents[1] / "tools/configure.py"
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / ".env"
            for origin in ("http://user:password@server", "http://server\nINJECTED=yes",
                           "http://server/path", "https://server#fragment", "file:///tmp"):
                result = subprocess.run([sys.executable, str(tool), "--origin", origin,
                                         "--output", str(output)], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(output.exists())
