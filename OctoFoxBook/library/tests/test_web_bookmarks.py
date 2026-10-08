import json
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from octofox_library.books_web import LibraryWeb, Session, WebError, WebServer


class BookmarkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'web.sqlite3'
        self.app = LibraryWeb(self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        for owner in ('alice', 'bob'):
            for book in ('42', '43'):
                self.app._index_batch([(owner, book, 'title', 'title', 'author', '', '', json.dumps({'id': book, 'title': 'Title', 'author': 'Author'}))])
        self.mark = {'action': 'add', 'chapter': 2, 'anchor': {'block': 3, 'char': 25}, 'label': 'Любимый момент', 'excerpt': 'Текст страницы'}

    def test_independent_durable_owner_book_scoped_crud(self):
        self.app.save_state('alice', '42', {'chapter': 1, 'offset': .4, 'anchor': {'block': 1, 'char': 4}})
        before = self.app.book('alice', '42')['reading']
        result = self.app.bookmarks('alice', '42', self.mark)['bookmarks']
        self.assertEqual(len(result), 1)
        identity = result[0]['id']
        self.assertEqual(self.app.bookmarks('alice', '42', self.mark)['bookmarks'], result, 'Retry is idempotent')
        self.assertEqual(self.app.book('alice', '42')['reading'], before)
        self.assertEqual(self.app.bookmarks('bob', '42')['bookmarks'], [])
        self.assertEqual(self.app.bookmarks('alice', '43')['bookmarks'], [])
        for owner, book in [('bob', '42'), ('alice', '43')]:
            for action in ['rename', 'remove']:
                with self.assertRaises(WebError) as error:
                    self.app.bookmarks(owner, book, {'action': action, 'id': identity, 'label': 'Attack'})
                self.assertEqual(error.exception.status, 404)
        self.app.save_state('alice', '42', {'chapter': 9, 'anchor': {'block': 10, 'char': 0}})
        self.assertEqual(self.app.bookmarks('alice', '42')['bookmarks'], result)
        restarted = LibraryWeb(self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        self.assertEqual(restarted.bookmarks('alice', '42')['bookmarks'], result)
        renamed = restarted.bookmarks('alice', '42', {'action': 'rename', 'id': identity, 'label': 'Новое имя'})['bookmarks'][0]
        self.assertEqual(renamed['label'], 'Новое имя')
        self.assertEqual(renamed['anchor'], self.mark['anchor'])
        self.assertEqual(restarted.bookmarks('alice', '42', {'action': 'remove', 'id': identity})['bookmarks'], [])
        self.assertEqual(restarted.book('alice', '42')['reading']['chapter'], 9)

    def test_validation(self):
        for delta in [{'chapter': -1}, {'chapter': True}, {'label': ''}, {'label': 'x' * 121},
                      {'anchor': {'block': 1, 'char': -1}}, {'anchor': None}, {'excerpt': 'x' * 241}, {'action': 'drop'}]:
            with self.subTest(delta=delta), self.assertRaises(WebError):
                self.app.bookmarks('alice', '42', self.mark | delta)
        self.assertEqual(self.app.bookmarks('alice', '42')['bookmarks'], [])

    def test_http_auth_csrf_and_served_asset(self):
        server = WebServer(('127.0.0.1', 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.app.sessions['token'] = Session('alice', 'Basic test', 'csrf-test', time.time() + 1000)
        base = f'http://127.0.0.1:{server.server_port}'
        def request(data=None, headers=None, path='/reader-api/books/42/bookmarks'):
            return urlopen(Request(base + path, data=None if data is None else json.dumps(data).encode(),
                                   headers={'Content-Type': 'application/json'} | (headers or {})), timeout=3)
        with patch.object(self.app, 'authenticate', return_value='alice'):
            with self.assertRaises(HTTPError) as error:
                request()
            self.assertEqual(error.exception.code, 401)
            headers = {'Cookie': 'books_session=token'}
            with self.assertRaises(HTTPError) as error:
                request(self.mark, headers)
            self.assertEqual(error.exception.code, 403)
            with request(self.mark | {'owner': 'bob'}, headers | {'X-CSRF-Token': 'csrf-test'}) as response:
                self.assertEqual(len(json.load(response)['bookmarks']), 1)
            with request(headers=headers) as response:
                self.assertEqual(len(json.load(response)['bookmarks']), 1)
            self.assertEqual(self.app.bookmarks('bob', '42')['bookmarks'], [])
            with request(path='/bookmarks.js?v=1') as response:
                self.assertIn(b'class Bookmarks', response.read())
