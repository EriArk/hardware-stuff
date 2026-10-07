import json
import runpy
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.request import Request, urlopen
from urllib.error import HTTPError

from octofox_library.books_web import LibraryWeb, Session, WebError, WebServer
from test_web_device_contents import PAYLOAD


class PersonalCollectionTests(unittest.TestCase):
    def test_entrypoint_and_imports_share_http_error_type(self):
        import octofox_library.books_web as web
        namespace = runpy.run_path(web.__file__, run_name='web_entrypoint_test')
        self.assertIs(namespace['WebError'], WebError)
        entry_app = namespace['LibraryWeb'](self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        with self.assertRaises(WebError) as error:
            entry_app.personal_collections('alice', {'action': 'delete', 'id': 'missing'})
        self.assertEqual(error.exception.status, 404)
        with self.assertRaises(WebError) as error:
            entry_app.upload_book('alice', b'not XML')
        self.assertEqual(error.exception.status, 422)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'web.sqlite3'
        self.app = LibraryWeb(self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        self.session = Session('alice', 'Basic test', 'csrf', time.time() + 1000)
        for owner in ('alice', 'bob'):
            for book in ('42', '43'):
                self.app._index_batch([(owner, book, 'title', 'title', 'author', '', '', json.dumps({'id': book, 'title': 'Title', 'author': 'Author'}))])
        index = patch.object(self.app, 'ensure_index'); index.start(); self.addCleanup(index.stop)

    def create(self, name='Любимое', owner='alice'):
        return self.app.personal_collections(owner, {'action': 'create', 'name': name})['id']

    def assign(self, identity, book='42', selected=True, owner='alice'):
        return self.app.book_collections(owner, book, {'id': identity, 'selected': selected})

    def test_crud_membership_durability_and_no_progress_or_device_changes(self):
        a, b = self.create(), self.create('На вечер')
        self.assertEqual(self.create('  ЛЮБИМОЕ  '), a)
        self.app.save_state('alice', '42', {'chapter': 2, 'offset': .4})
        before = self.app.book('alice', '42')['reading']
        self.assertTrue(self.assign(a)['inLibrary'])
        self.assign(a); self.assign(b)
        self.assertEqual([r['count'] for r in self.app.personal_collections('alice')['collections']], [1, 1])
        restart = LibraryWeb(self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        self.assertEqual(sum(c['selected'] for c in restart.book_collections('alice', '42')['collections']), 2)
        self.app.personal_collections('alice', {'action': 'rename', 'id': a, 'name': 'Романы'})
        self.assign(b, selected=False)
        self.assertTrue(self.app.book('alice', '42')['inLibrary'])
        self.app.personal_collections('alice', {'action': 'delete', 'id': a})
        self.assertTrue(self.app.book('alice', '42')['inLibrary'])
        self.assertEqual(self.app.book('alice', '42')['reading'], before)
        with self.app.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM delivery').fetchone()[0], 0)
        self.assign(b)
        self.app.save_book('alice', '42', False)
        self.assertFalse(self.app.book_collections('alice', '42')['collections'][0]['selected'])
        self.assertEqual(self.app.personal_collections('alice')['collections'][0]['count'], 0)

    def test_isolation_validation_private_upload_and_catalog_filters(self):
        a = self.create()
        self.assign(a)
        for action in ('rename', 'delete'):
            with self.assertRaises(WebError):
                self.app.personal_collections('bob', {'action': action, 'id': a, 'name': 'X'})
        with self.assertRaises(WebError): self.assign(a, owner='bob')
        for data in ({'action': 'create', 'name': ''}, {'action': 'create', 'name': 'x' * 81}, {'action': 'drop'}, []):
            with self.assertRaises(WebError): self.app.personal_collections('alice', data)
        with self.assertRaises(WebError): self.app.book_collections('alice', '42', {'id': a, 'selected': 'yes'})
        upload = self.app.upload_book('alice', PAYLOAD)
        with self.assertRaises(WebError): self.app.book_collections('bob', upload['id'])
        self.assign(a, upload['id'])
        query = {'personalCollection': [a]}
        result = self.app.catalog(self.session, query)
        self.assertEqual({r['id'] for r in result['books']}, {'42', upload['id']})
        self.assertEqual(self.app.catalog(self.session, query | {'q': ['zzzz']})['total'], 0)
        self.assertEqual(self.app.catalog(self.session, query | {'page': ['2']})['books'], [])
        with self.assertRaises(WebError):
            self.app.catalog(Session('bob', '', '', 0), query)
        other = self.create('Other')
        with self.assertRaises(WebError): self.app.personal_collections('alice', {'action': 'rename', 'id': other, 'name': 'Любимое'})

    def test_http_auth_csrf_and_owner_derived_from_session(self):
        server = WebServer(('127.0.0.1', 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        self.app.sessions['test-token'] = self.session
        base = f'http://127.0.0.1:{server.server_port}'
        def request(path, data=None, headers=None):
            return urlopen(Request(base + path, data=None if data is None else json.dumps(data).encode(),
                                  headers={'Content-Type': 'application/json'} | (headers or {})), timeout=3)
        headers = {'Cookie': 'books_session=test-token'}
        with patch.object(self.app, 'authenticate', return_value='alice'):
            with self.assertRaises(HTTPError) as e: request('/reader-api/personal-collections')
            self.assertEqual(e.exception.code, 401)
            with self.assertRaises(HTTPError) as e: request('/reader-api/personal-collections', {'action':'create','name':'A'}, headers)
            self.assertEqual(e.exception.code, 403)
            headers['X-CSRF-Token'] = 'csrf'
            with request('/reader-api/personal-collections', {'action':'create','name':'A','owner':'bob'}, headers) as response:
                identity = json.load(response)['id']
            with request('/reader-api/books/42/personal-collections', {'id': identity, 'selected': True}, headers) as response:
                self.assertTrue(json.load(response)['inLibrary'])
            self.assertEqual(self.app.personal_collections('bob')['collections'], [])
            with request('/personal-library.js?v=1') as response:
                self.assertIn(b'uploadBatch', response.read())
