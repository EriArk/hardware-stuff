import unittest
import test_books_web
from octofox_library.books_web import WebError


class ReaderProfileSyncTests(unittest.TestCase):
    setUp = test_books_web.BooksWebTest.setUp
    seed = test_books_web.BooksWebTest.seed

    def test_offline_favorite_uploaded_before_poll_prevents_removal(self):
        self.app.enqueue('alice', '42', 'default')
        wire = self.app.poll('alice', 'reader-aabbcc', 2)
        self.app.acknowledge('alice', 'reader-aabbcc', int(wire.split(b'\t')[1]), 'delivered', 'a'*64)
        self.app.save_book('alice', '42', False)
        self.app.reader_collections('alice', 'reader-aabbcc', {
            'op': 'a'*32, 'action': 'member', 'id': 'favorite', 'book': 'opds-42', 'selected': True})
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2), b'')
        self.assertIn(['m', 'favorite', 'opds-42'], self.app.reader_collections('alice', 'reader-aabbcc')['records'])

    def test_default_profile_addition_removal_and_manual_override(self):
        self.assertTrue(self.app.devices('alice')[0]['profile_sync'])
        self.app.save_book('alice', '42', True)
        self.assertEqual(self.app.deliveries('alice'), [])  # No background network work.
        wire = self.app.poll('alice', 'reader-aabbcc', 2)
        self.assertIn(b'\topds-42\t', wire)
        job = int(wire.split(b'\t')[1])
        self.app.acknowledge('alice', 'reader-aabbcc', job, 'delivered', 'a'*64)
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2), b'')
        with self.assertRaises(WebError): self.app.remove_from_device('alice', job)
        self.app.save_book('alice', '42', False)
        wire = self.app.poll('alice', 'reader-aabbcc', 2)
        self.assertTrue(wire.startswith(b'2\t')); self.assertTrue(wire.endswith(b'\tremove\n'))
        remove = int(wire.split(b'\t')[1]); self.assertNotEqual(job, remove)
        with self.assertRaises(WebError): self.app.acknowledge('alice', 'reader-aabbcc', job, 'delivered', 'a'*64)
        self.app.acknowledge('alice', 'reader-aabbcc', remove, 'removed')
        self.app.set_profile_sync('alice', 'reader-aabbcc', {'enabled': False})
        self.app.save_book('alice', '42', True)
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2), b'')
        self.assertFalse(self.app.devices('alice')[0]['profile_sync'])
        self.app.enqueue('alice', '42', 'reader-aabbcc')
        self.assertTrue(self.app.poll('alice', 'reader-aabbcc', 2).startswith(b'1\t'))

    def test_default_placeholder_setting_transfers_and_is_account_scoped(self):
        self.app.set_profile_sync('alice', 'default', {'enabled': False})
        self.app.save_book('alice', '42', True)
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2), b'')
        self.assertFalse(self.app.devices('alice')[0]['profile_sync'])
        self.assertTrue(self.app.devices('bob')[0]['profile_sync'])
        for data in ({'enabled': 'false'}, {}, [], {'enabled': 0}):
            with self.assertRaises(WebError): self.app.set_profile_sync('alice', 'reader-aabbcc', data)
        with self.assertRaises(WebError): self.app.set_profile_sync('bob', 'reader-aabbcc', {'enabled': False})
        self.app.set_profile_sync('alice', 'reader-aabbcc', {'enabled': True})
        self.assertIn(b'\topds-42\t', self.app.poll('alice', 'reader-aabbcc', 2))

    def test_existing_lease_is_stable_and_cursor_does_not_reconcile_mid_pass(self):
        self.app.save_book('alice', '42', True)
        first = self.app.poll('alice', 'reader-aabbcc', 2)
        job = int(first.split(b'\t')[1])
        self.app.acknowledge('alice', 'reader-aabbcc', job, 'queued', error='storage')
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2), first)
        self.app.save_book('alice', '42', False)
        self.assertEqual(self.app.poll('alice', 'reader-aabbcc', 2, after=job), b'')
        self.assertTrue(self.app.poll('alice', 'reader-aabbcc', 2).startswith(b'2\t'))
