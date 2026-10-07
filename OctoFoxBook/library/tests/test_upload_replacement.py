import hashlib
import re
import unittest
from unittest.mock import patch

import test_books_web as helpers
from test_web_device_contents import PAYLOAD
from octofox_library.books_web import LibraryWeb, WebError
from octofox_library.book_formats import normalize_fb2
from octofox_library.web_uploads import MAX_ACCOUNT_UPLOAD_BYTES, MAX_ACCOUNT_UPLOAD_BOOKS


class ReplacementTest(unittest.TestCase):
    setUp = helpers.BooksWebTest.setUp
    seed = helpers.BooksWebTest.seed

    def test_auto_collection_and_filename_replacement_preserve_identity_and_shelves(self):
        first = self.app.upload_book('alice', PAYLOAD, filename='Book.fb2', modified=100)
        book = first['id']
        self.assertEqual(first['uploadResult'], 'added')
        collection = self.app.personal_collections('alice')['collections'][0]
        self.assertEqual((collection['name'], collection['count']), ('Загруженные', 1))
        self.app.save_state('alice', book, {'chapter':0, 'offset':.5})
        self.app.favorite_book('alice', book, True)
        self.app.chapters(self.session, book)
        old = self.app.book('alice', book)
        larger = PAYLOAD.replace(b'</section>', '<p>Дополнительный текст.</p></section>'.encode())
        update = self.app.upload_book('alice', larger, filename='BOOK.FB2', modified=50)
        self.assertEqual((update['id'], update['uploadResult']), (book, 'replaced'))
        self.assertEqual(update['reading'], old['reading'])
        self.assertTrue(update['isFavorite'])
        self.assertIn('Дополнительный', self.app.chapters(self.session, book)[0]['html'])
        self.assertEqual(self.app.personal_collections('alice')['collections'][0]['count'], 1)
        kept = self.app.upload_book('alice', PAYLOAD, filename='book.fb2', modified=49)
        self.assertEqual(kept['uploadResult'], 'kept')
        self.assertEqual(self.app.upload_path('alice', book).read_bytes(), larger)
        # A genuinely newer but smaller edition replaces as requested.
        newer = self.app.upload_book('alice', PAYLOAD, filename='book.fb2', modified=200)
        self.assertEqual((newer['id'], newer['uploadResult']), (book, 'replaced'))
        duplicate = self.app.upload_book('alice', PAYLOAD, filename='other.fb2', modified=201)
        self.assertEqual((duplicate['id'], duplicate['uploadResult']), (book, 'duplicate'))
        other = self.app.upload_book('bob', PAYLOAD, filename='book.fb2', modified=202)
        self.assertNotEqual(other['id'], book)
        with self.app.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM uploaded_books WHERE owner=?', ('alice',)).fetchone()[0], 1)

    def test_failed_replacement_restores_file_metadata_and_original(self):
        first = self.app.upload_book('alice', PAYLOAD, filename='b.fb2', modified=10)
        book = first['id']
        path = self.app.upload_path('alice', book)
        newer = PAYLOAD.replace(b'</section>', b'<p>New</p></section>')
        with patch.object(self.app, '_write_terms', side_effect=OSError('disk failure')):
            with self.assertRaises(OSError):
                self.app.upload_book('alice', newer, filename='b.fb2', modified=20)
        self.assertEqual(path.read_bytes(), PAYLOAD)
        self.assertEqual(self.app.book('alice', book)['title'], first['title'])
        self.assertEqual(list(path.parent.glob('*.backup')), [])
        self.assertEqual(list(path.parent.glob('*.part')), [])
        with self.app.db() as db:
            self.assertEqual(db.execute('SELECT digest FROM uploaded_books WHERE book=?', (book,)).fetchone()[0], hashlib.sha256(PAYLOAD).hexdigest())

    def test_backfill_only_current_shelf_and_idempotent_restart(self):
        one = self.app.upload_book('alice', PAYLOAD)['id']
        two = self.app.upload_book('bob', PAYLOAD)['id']
        self.app.save_book('bob', two, False)
        with self.app.db() as db:
            db.execute('DELETE FROM personal_collection_books')
            db.execute('DELETE FROM personal_collections')
            db.execute('DELETE FROM web_schema WHERE version=6')
        for _ in range(2):
            reopened = LibraryWeb(self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
            self.assertEqual(reopened.personal_collections('alice')['collections'][0]['count'], 1)
            self.assertEqual(reopened.personal_collections('bob')['collections'], [])
            self.assertEqual(reopened.book('alice', one)['id'], one)

    def test_inline_repair_preserves_all_text_and_original_source(self):
        bad = PAYLOAD.replace(b'</section>', b'<p><strong>A</p><p>B</strong></p><p>C</strong></p></section>')
        good, fixes = normalize_fb2(bad)
        self.assertIn('xml-inline-paragraphs', fixes)
        self.assertEqual(re.sub(rb'<[^>]+>', b'', bad), re.sub(rb'<[^>]+>', b'', good))
        book = self.app.upload_book('alice', bad, filename='b.fb2')
        path = self.app.upload_path('alice', book['id'])
        self.assertEqual(path.read_bytes(), good)
        self.assertEqual(path.with_suffix('.original.source').read_bytes(), bad)
        for broken in (bad[:-20], bad.replace(b'</body>', b'</broken>')):
            with self.assertRaises(ValueError):
                normalize_fb2(broken)

    def test_no_user_filename_used_as_disk_path(self):
        for name in ('../x.fb2', 'a/b.fb2', 'x\x00.fb2', 'a\\b.fb2'):
            with self.assertRaises(WebError):
                self.app.upload_book('alice', PAYLOAD, filename=name)

    def test_three_gib_quota_boundary_duplicates_and_account_isolation(self):
        self.assertEqual(MAX_ACCOUNT_UPLOAD_BYTES, 3 * 1024**3)
        first = self.app.upload_book('alice', PAYLOAD, filename='one.fb2')
        second_payload = PAYLOAD.replace(b'</section>', b'<p>Second book</p></section>')
        # Model an almost-full account without allocating gigabytes in a unit test.
        with self.app.db() as db:
            db.execute('UPDATE uploaded_books SET bytes=? WHERE owner=? AND book=?',
                       (MAX_ACCOUNT_UPLOAD_BYTES - len(second_payload), 'alice', first['id']))
        second = self.app.upload_book('alice', second_payload, filename='two.fb2')
        self.assertEqual(second['uploadResult'], 'added')
        with self.app.db() as db:
            self.assertEqual(db.execute('SELECT sum(bytes) FROM uploaded_books WHERE owner=?',
                                       ('alice',)).fetchone()[0], MAX_ACCOUNT_UPLOAD_BYTES)
        third_payload = PAYLOAD.replace(b'</section>', b'<p>Third book</p></section>')
        with self.assertRaises(WebError) as error:
            self.app.upload_book('alice', third_payload, filename='three.fb2')
        self.assertEqual(error.exception.status, 413)
        self.assertIn('3 ГБ', error.exception.message)
        self.assertEqual(self.app.upload_book('alice', second_payload)['uploadResult'], 'duplicate')
        self.assertEqual(self.app.upload_book('bob', third_payload)['uploadResult'], 'added')

    def test_1500_book_boundary_replacement_duplicates_and_other_account(self):
        self.assertEqual(MAX_ACCOUNT_UPLOAD_BOOKS, 1500)
        # Count-only fixture rows avoid writing 1499 unnecessary files.
        with self.app.db() as db:
            db.executemany('INSERT INTO uploaded_books VALUES (?,?,?,?,?)',
                           [('alice', str(800000000000 + n), f'fixture-{n}', 256, 1)
                            for n in range(1499)])
        last = self.app.upload_book('alice', PAYLOAD, filename='last.fb2', modified=1)
        self.assertEqual(last['uploadResult'], 'added')
        more = PAYLOAD.replace(b'</section>', b'<p>More text</p></section>')
        with self.assertRaises(WebError) as error:
            self.app.upload_book('alice', more, filename='extra.fb2')
        self.assertEqual(error.exception.status, 413)
        self.assertIn('1500', error.exception.message)
        self.assertEqual(self.app.upload_book('alice', PAYLOAD)['uploadResult'], 'duplicate')
        replacement = self.app.upload_book('alice', more, filename='last.fb2', modified=2)
        self.assertEqual((replacement['id'], replacement['uploadResult']), (last['id'], 'replaced'))
        with self.app.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM uploaded_books WHERE owner=?',
                                       ('alice',)).fetchone()[0], 1500)
        self.assertEqual(self.app.upload_book('bob', more)['uploadResult'], 'added')


if __name__ == '__main__':
    unittest.main()
