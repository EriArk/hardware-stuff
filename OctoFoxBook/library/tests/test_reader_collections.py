import json
import secrets
import threading
from unittest.mock import patch
from urllib.request import Request, urlopen
from urllib.error import HTTPError

from test_web_personal_collections import PersonalCollectionTests
from octofox_library.books_web import WebError, WebServer
from octofox_library.web_reader_collections import encoded


class ReaderCollectionsTests(PersonalCollectionTests):
    def op(self, action, identity, **fields):
        return dict(op=secrets.token_hex(16), action=action, id=identity, **fields)

    def test_retry_alias_multi_membership_and_favorite_independence(self):
        first = self.create('На вечер')
        create = self.op('create', 'local-id', name='На вечер')
        for _ in range(2):
            self.assertEqual(self.app.reader_collections('alice', 'reader-a', create)['id'], first)
        second = self.app.reader_collections('alice', 'reader-a', self.op('create', 'other-id', name='Other'))['id']
        for identity in (first, second, 'favorite'):
            operation = self.op('member', identity, book='opds-42', selected=True)
            for _ in range(2): self.app.reader_collections('alice', 'reader-a', operation)
        self.assertEqual(sum(r['selected'] for r in self.app.book_collections('alice', '42')['collections']), 2)
        self.app.reader_collections('alice', 'reader-a', self.op('member', 'favorite', book='opds-42', selected=False))
        self.assertEqual(sum(r['selected'] for r in self.app.book_collections('alice', '42')['collections']), 2)
        with self.app.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM favorite_books').fetchone()[0], 0)
            self.assertEqual(db.execute('SELECT count(*) FROM delivery').fetchone()[0], 0)
        with self.assertRaises(WebError):
            self.app.reader_collections('alice', 'reader-a', create | {'name': 'Changed'})

    def test_account_boundaries_validation_and_bounded_pages(self):
        identity = self.create()
        with self.assertRaises(WebError):
            self.app.reader_collections('bob', 'reader-a', self.op('member', identity, book='opds-42', selected=True))
        for payload in ([], {}, self.op('member', 'favorite', book='opds-42', selected='true'),
                        self.op('create', 'favorite', name='X'), self.op('create', 'id', name=' ')):
            with self.assertRaises(WebError): self.app.reader_collections('alice', 'reader-a', payload)
        for i in range(40): self.create(f'{i:02d} ' + 'Ч' * 70)
        cursor = 0; records = []; revisions = set()
        while True:
            page = self.app.reader_collections('alice', 'reader-a', offset=cursor)
            self.assertLessEqual(len(encoded(page)), 1900)
            records.extend(page['records']); revisions.add(page['revision'])
            if not page['next']: break
            self.assertGreater(page['next'], cursor); cursor = page['next']
        self.assertEqual(len(records), 42)
        self.assertEqual(len(revisions), 1)
        self.assertNotEqual(page['account'], self.app.reader_collections('bob', 'reader-a')['account'])
        for offset in (-1, 40000, 'x'):
            with self.assertRaises(WebError): self.app.reader_collections('alice', 'reader-a', offset=offset)

    def test_snapshot_contains_only_books_present_on_this_device(self):
        identity = self.create(); self.assign(identity)
        with self.app.db() as db:
            db.execute("INSERT INTO devices VALUES ('alice','reader-a','Reader',0)")
        self.app.enqueue('alice', '42', 'reader-a')
        with self.app.db() as db:
            db.execute("UPDATE delivery SET present=1 WHERE owner='alice' AND device='reader-a' AND book='42'")
        a = self.app.reader_collections('alice', 'reader-a')
        b = self.app.reader_collections('alice', 'reader-b')
        self.assertIn(['m', identity, 'opds-42'], a['records'])
        self.assertNotIn(['m', identity, 'opds-42'], b['records'])
        self.assertEqual(a['account'], b['account'])

    def test_device_http_auth_and_compact_json(self):
        server = WebServer(('127.0.0.1', 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        url = f'http://127.0.0.1:{server.server_port}/reader-api/device/collections?device=reader-a'
        with patch.object(self.app, 'authenticate', return_value='alice') as auth:
            request = Request(url, encoded(self.op('create', 'local-id', name='Русская коллекция')),
                              headers={'Authorization': 'Basic device-test', 'Content-Type': 'application/json'})
            with urlopen(request, timeout=3) as response: self.assertEqual(json.load(response)['id'], 'local-id')
            with urlopen(Request(url, headers={'Authorization': 'Basic device-test'}), timeout=3) as response:
                body = response.read(); self.assertLessEqual(len(body), 1900)
                self.assertIn(['s', 'local-id', 'Русская коллекция'], json.loads(body)['records'])
                auth.assert_called_with('Basic device-test', device_allowed=True, device='reader-a')
        with patch.object(self.app, 'authenticate', side_effect=WebError(401, 'Unauthorized')):
            with self.assertRaises(HTTPError) as error: urlopen(url, timeout=3)
            self.assertEqual(error.exception.code, 401)
