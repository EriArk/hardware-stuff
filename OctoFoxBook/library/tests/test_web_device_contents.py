import base64
import json
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

import test_books_web as helpers
from octofox_library.books_web import LibraryWeb, WebError, WebServer
from octofox_library.web_uploads import MAX_UPLOAD, parse_upload


PAYLOAD = ('<?xml version="1.0" encoding="utf-8"?>'
           '<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">'
           '<description><title-info><genre>sf_fantasy</genre><author>'
           '<first-name>Автор</first-name><last-name>Тест</last-name></author>'
           '<book-title>Личная книга</book-title><lang>ru</lang></title-info></description>'
           '<body><section><title><p>Начало</p></title><p>Текст личной книги.</p>'
           '</section></body></FictionBook>').encode()


class DeviceContentsTest(unittest.TestCase):
    def setUp(self):
        helpers.BooksWebTest.setUp(self)
        # These scenarios explicitly exercise the optional manual device shelf.
        self.app.set_profile_sync('alice', 'default', {'enabled': False})
    seed = helpers.BooksWebTest.seed

    def test_manual_pass_cursor_skips_failure_but_next_pass_retries_it(self):
        first = self.app.enqueue('alice', '42', 'default')['id']
        self.app.poll('alice', 'reader-aabbcc', 2)
        self.app.acknowledge('alice', 'reader-aabbcc', first, 'queued', error='sd-write-failed')
        # A later removal for another book is not blocked by the failed head.
        with self.app.db() as db:
            row = db.execute('SELECT record FROM books WHERE owner=? AND id=?', ('alice', '42')).fetchone()
            record = json.loads(row[0]); record['id'] = '43'
            db.execute('INSERT INTO books SELECT owner,?,title,search,author,series,updated,? FROM books WHERE owner=? AND id=?',
                       ('43', json.dumps(record), 'alice', '42'))
        second = self.app.enqueue('alice', '43', 'reader-aabbcc')['id']
        removal = self.app.remove_from_device('alice', second)['id']
        self.assertTrue(self.app.poll('alice', 'reader-aabbcc', 2, after=first).startswith(f'2\t{removal}\t'.encode()))
        self.app.acknowledge('alice', 'reader-aabbcc', removal, 'removed')
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2, after=removal), b'')
        self.assertTrue(self.app.poll('alice', 'reader-aabbcc', 2).startswith(f'1\t{first}\t'.encode()))

    def test_delivery_cursor_is_validated_before_database_writes(self):
        for cursor in (-1, 2**63, True, '1 OR 1=1'):
            with self.assertRaises(WebError):
                self.app.poll('alice', 'reader-aabbcc', 2, after=cursor)

    def delivered(self):
        job = self.app.enqueue('alice', '42', 'default')['id']
        self.app.poll('alice', 'reader-aabbcc')
        self.app.acknowledge('alice', 'reader-aabbcc', job, 'delivered', 'a' * 64)
        return job

    def test_inventory_remove_waits_for_v2_ack_and_preserves_source(self):
        job = self.delivered()
        self.assertEqual(self.app.deliveries('alice')[0]['present'], 1)
        self.app.save_book('alice', '42', True)
        removal = self.app.remove_from_device('alice', job)
        self.assertNotEqual(removal['id'], job)
        self.assertEqual(removal['present'], 1)
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc'), b'')
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2),
                         f"2\t{removal['id']}\topds-42\tremove\n".encode())
        with self.assertRaises(WebError):
            self.app.acknowledge('alice', 'reader-aabbcc', removal['id'], 'delivered', 'b'*64)
        self.app.acknowledge('alice', 'reader-aabbcc', removal['id'], 'removed')
        self.app.acknowledge('alice', 'reader-aabbcc', removal['id'], 'removed')
        self.assertEqual(self.app.deliveries('alice'), [])
        self.assertTrue(self.app.book('alice', '42')['inLibrary'])
        new = self.app.enqueue('alice', '42', 'reader-aabbcc')
        self.assertGreater(new['id'], removal['id'])
        self.assertEqual(new['present'], 0)
        self.assertEqual(new['action'], 'download')

    def test_intent_ids_fence_late_receipts_and_are_owner_scoped(self):
        job = self.delivered()
        with self.assertRaises(WebError):
            self.app.remove_from_device('bob', job)
        removal = self.app.remove_from_device('alice', job)
        self.app.poll('alice', 'reader-aabbcc', 2)
        new = self.app.enqueue('alice', '42', 'reader-aabbcc')
        for old, state in [(job, 'delivered'), (removal['id'], 'removed')]:
            with self.assertRaises(WebError):
                self.app.acknowledge('alice', 'reader-aabbcc', old, state, 'a'*64)
        self.assertEqual(self.app.deliveries('alice')[0]['id'], new['id'])
        self.assertEqual(self.app.deliveries('alice')[0]['state'], 'queued')

    def test_failed_removal_retry_gets_new_lease(self):
        row = self.app.remove_from_device('alice', self.delivered())
        self.app.poll('alice', 'reader-aabbcc', 2)
        self.app.acknowledge('alice', 'reader-aabbcc', row['id'], 'failed', error='sd-remove-failed')
        retry = self.app.remove_from_device('alice', row['id'])
        self.assertGreater(retry['id'], row['id'])
        self.assertEqual(retry['present'], 1)
        self.assertEqual(retry['state'], 'queued')

    def test_upload_private_deduplicated_readable_and_survives_restart(self):
        book = self.app.upload_book('alice', PAYLOAD)
        self.assertTrue(book['inLibrary'])
        self.assertTrue(book['privateUpload'])
        self.assertEqual(book['author'], 'Автор Тест')
        book_id = book['id']
        self.app.save_state('alice', book_id, {'chapter': 0, 'offset': .6})
        self.assertEqual(self.app.upload_book('alice', PAYLOAD)['id'], book_id)
        self.assertEqual(self.app.book('alice', book_id)['reading']['offset'], .6)
        self.assertEqual(self.app.catalog(self.session, {'personal':['1']})['total'], 1)
        self.assertEqual(self.app.catalog(self.session, {})['total'], 1) # only shared 42
        self.assertEqual(self.app.catalog(self.session, {'new':['1']})['total'], 1)
        with self.assertRaises(WebError):
            self.app.book('bob', book_id)
        self.assertIsNone(self.app.upload_path('bob', book_id))
        self.assertIn('Текст личной книги', self.app.chapters(self.session, book_id)[0]['html'])
        self.assertEqual(self.app.download_book(self.session, book_id)[0], PAYLOAD)
        self.app.enqueue('alice', book_id, 'default')
        self.assertIn(f'/reader-api/uploads/{book_id}/download'.encode(), self.app.poll('alice', 'reader-aabbcc'))
        reopened = LibraryWeb(self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        self.assertEqual(reopened.upload_path('alice', book_id).read_bytes(), PAYLOAD)
        self.assertEqual(reopened.book('alice', book_id)['reading']['offset'], .6)

    def test_index_refresh_preserves_private_book(self):
        book = self.app.upload_book('alice', PAYLOAD)
        with patch.object(self.app.client, 'iter_catalog_entries', return_value=iter([])):
            self.app._index(self.session)
        self.assertTrue(self.app.book('alice', book['id'])['privateUpload'])

    def test_upload_rejects_dtd_entities_bad_root_and_size(self):
        for payload in [b'', b'<html>'+b' '*400+b'</html>', b'PK'+b' '*500,
                        b'<!DOCTYPE x>'+PAYLOAD, (b'<!ENTITY x "bad">'+PAYLOAD).decode().encode('utf-16'),
                        PAYLOAD.replace(b'book-title', b'missing-title')]:
            with self.assertRaises(ValueError):
                parse_upload(payload)
        with patch('octofox_library.web_uploads.MAX_UPLOAD', 300):
            with self.assertRaises(ValueError):
                parse_upload(PAYLOAD)

    def test_upload_cover_and_invalid_cover_fallback(self):
        png = b'\x89PNG\r\n\x1a\n' + b'png-fixture'
        cover = b'<coverpage><image href="#image"/></coverpage>'
        data = PAYLOAD.replace(b'<lang>', cover+b'<lang>').replace(b'</FictionBook>',
            b'<binary id="image">'+base64.b64encode(png)+b'</binary></FictionBook>')
        book = self.app.upload_book('alice', data)
        self.assertEqual(self.app.upload_cover('alice', book['id']), (png, 'image/png'))
        data = data.replace(base64.b64encode(png), base64.b64encode(b'<svg><script/></svg>'))
        self.assertEqual(parse_upload(data)[1], b'')

    def test_http_upload_csrf_ownership_and_device_download(self):
        server = WebServer(('127.0.0.1', 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.app.origin = base = f'http://127.0.0.1:{server.server_port}'
        self.app.sessions['token'] = self.session
        headers = {'Cookie':'books_session=token', 'X-CSRF-Token':'csrf-test',
                   'Origin':base, 'Content-Type':'application/octet-stream'}
        with patch.object(self.app, 'authenticate', return_value='alice'):
            for extra, expected in [({'X-CSRF-Token':''},403), ({'Origin':'http://evil.test'},403),
                                     ({'Content-Type':'text/html'},415)]:
                with self.assertRaises(HTTPError) as error:
                    urlopen(Request(base+'/reader-api/uploads', data=PAYLOAD, headers=headers|extra))
                self.assertEqual(error.exception.code, expected)
            with urlopen(Request(base+'/reader-api/uploads', data=PAYLOAD, headers=headers)) as response:
                self.assertEqual(response.status, 201)
                book_id = json.load(response)['id']
            # More than the old 12/minute limit; identical file retries remain idempotent.
            for _ in range(13):
                with urlopen(Request(base+'/reader-api/uploads', data=PAYLOAD, headers=headers)) as response:
                    self.assertEqual(json.load(response)['id'], book_id)
            asset = base+f'/reader-api/uploads/{book_id}/download'
            with self.assertRaises(HTTPError) as error:
                urlopen(asset)
            self.assertEqual(error.exception.code, 401)
            with urlopen(Request(asset, headers={'Authorization':'Basic test'})) as response:
                self.assertEqual(response.read(), PAYLOAD)
                self.assertIn('attachment', response.headers['Content-Disposition'])
            with patch.object(self.app, 'authenticate', return_value='bob'):
                with self.assertRaises(HTTPError) as error:
                    urlopen(Request(asset, headers={'Authorization':'Basic other'}))
                self.assertEqual(error.exception.code, 404)
            self.app.enqueue('alice', book_id, 'default')
            row = self.app.deliveries('alice')[0]
            self.app.remove_from_device('alice', row['id'])
            with urlopen(Request(base+'/reader-api/device/next?device=reader-aabbcc&v=2', headers={'Authorization':'Basic test'})) as response:
                self.assertIn(b'\tremove\n', response.read())


if __name__ == '__main__':
    unittest.main()
