"""Public decorative fallbacks cannot expose private files or affect API caching."""
import tempfile
import threading
import unittest
import io
from email.message import Message
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import urlopen

from octofox_library.books_web import LibraryWeb, STATIC, WebServer
import test_books_web


class CoverArtTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        app = LibraryWeb(Path(temp.name) / 'web.sqlite3', 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        server = WebServer(('127.0.0.1', 0), app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.base = f'http://127.0.0.1:{server.server_port}'

    def test_all_art_is_public_bounded_webp_and_immutable(self):
        digests = set()
        for number in range(1, 17):
            path = f'/cover-art/v1/{number:02d}.webp'
            data = (STATIC / path[1:]).read_bytes()
            self.assertEqual(data[:4], b'RIFF')
            self.assertEqual(data[8:12], b'WEBP')
            self.assertLess(len(data), 200_000)
            digests.add(data)
            with urlopen(self.base + path, timeout=5) as response:
                self.assertEqual(response.read(), data)
                self.assertEqual(response.headers.get_content_type(), 'image/webp')
                self.assertEqual(response.headers.get_all('Cache-Control'),
                                 ['public, max-age=31536000, immutable'])
                self.assertEqual(response.headers['X-Content-Type-Options'], 'nosniff')
        self.assertEqual(len(digests), 16)

    def test_unversioned_and_traversal_paths_are_not_served(self):
        for path in ('/cover-art/v1/00.webp', '/cover-art/v1/17.webp', '/cover-art/v2/01.webp',
                     '/cover-art/v1/01.png', '/cover-art/../books_web.py',
                     '/cover-art/v1/%2e%2e/%2e%2e/books_web.py'):
            with self.assertRaises(HTTPError) as error:
                urlopen(self.base + path, timeout=5)
            self.assertEqual(error.exception.code, 404)
            self.assertEqual(error.exception.headers['Cache-Control'], 'no-store')

    def test_cover_fonts_are_public_bounded_and_immutable(self):
        for subset in ('cyrillic', 'latin'):
            path = f'/fonts/cormorant-garamond-600-{subset}-v1.woff2'
            data = (STATIC / path[1:]).read_bytes()
            self.assertEqual(data[:4], b'wOF2')
            self.assertLess(len(data), 50_000)
            with urlopen(self.base + path, timeout=5) as response:
                self.assertEqual(response.read(), data)
                self.assertEqual(response.headers.get_content_type(), 'font/woff2')
                self.assertEqual(response.headers.get_all('Cache-Control'),
                                 ['public, max-age=31536000, immutable'])
        for path in ('/fonts/unknown.woff2', '/fonts/../books_web.py',
                     '/fonts/%2e%2e/books_web.py'):
            with self.assertRaises(HTTPError) as error:
                urlopen(self.base + path, timeout=5)
            self.assertEqual(error.exception.code, 404)

    def test_other_responses_remain_noncacheable(self):
        for path in ('/', '/app.js', '/healthz'):
            with urlopen(self.base + path, timeout=5) as response:
                self.assertEqual(response.headers.get_all('Cache-Control'), ['no-store'])
        with self.assertRaises(HTTPError) as error:
            urlopen(self.base + '/reader-api/me', timeout=5)
        self.assertEqual(error.exception.code, 401)
        self.assertEqual(error.exception.headers['Cache-Control'], 'no-store')


class UpstreamCoverTest(unittest.TestCase):
    setUp = test_books_web.BooksWebTest.setUp
    seed = test_books_web.BooksWebTest.seed
    def test_exact_booklore_placeholder_is_missing_but_other_images_are_unchanged(self):
        server = WebServer(('127.0.0.1', 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.app.sessions['test-token'] = self.session
        from urllib.request import Request
        request = Request(f'http://127.0.0.1:{server.server_port}/reader-api/books/42/cover',
                          headers={'Cookie': 'books_session=test-token'})
        bundled = (Path(__file__).parent / 'fixtures/booklore-missing-cover.jpg').read_bytes()
        # A different image of exactly the same byte length is not a placeholder.
        for data, missing in ((bundled, True), (bundled[:-1] + b'x', False)):
            upstream = io.BytesIO(data)
            upstream.headers = Message()
            upstream.headers['Content-Type'] = 'image/jpeg'
            with patch.object(self.app, 'authenticate', return_value='alice'), \
                    patch.object(self.app.client, 'open', return_value=upstream):
                if missing:
                    with self.assertRaises(HTTPError) as error:
                        urlopen(request, timeout=5)
                    self.assertEqual(error.exception.code, 404)
                else:
                    with urlopen(request, timeout=5) as response:
                        self.assertEqual(response.status, 200)
                        self.assertEqual(response.read(), data)
