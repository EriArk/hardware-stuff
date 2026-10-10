import hashlib
import secrets
import unittest
from unittest.mock import patch
import test_web_personal_collections as fixture
from octofox_library.books_web import fb2_chapters, WebError
from octofox_library.reader_anchors import AnchorMap
from octofox_library.web_reader_state import wire

BOOK = '''<?xml version="1.0" encoding="UTF-8"?><FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">
<description><title-info><book-title>Test</book-title><author><first-name>A</first-name><last-name>B</last-name></author></title-info></description>
<body><section><title><p>Chapter</p></title>
<p>  Привет <emphasis>мир 😀</emphasis>!   Again.</p><empty-line/><image href="#x"/>
<section><p>Next paragraph.</p><text-author>Author</text-author></section></section></body>
<body name="notes"><section><p>Footnote</p></section></body></FictionBook>'''.encode()


class AnchorTests(unittest.TestCase):
    def test_utf8_utf16_whitespace_inline_and_first_body_records(self):
        mapping = AnchorMap(fb2_chapters(BOOK))
        self.assertEqual(set(mapping.records), {2, 3, 7, 8})
        for record, value in mapping.records.items():
            normalized, _ = mapping.boundaries(value['text'])
            for offset in range(len(normalized)+1):
                byte = len(normalized[:offset].encode())
                chapter, anchor = mapping.to_web(record, byte)
                self.assertEqual(mapping.to_native(chapter, anchor), dict(record=record, byte=byte))
        with self.assertRaises(ValueError): mapping.to_web(3, 1)
        with self.assertRaises(ValueError): mapping.to_web(5, 0)
        with self.assertRaises(ValueError): mapping.to_web(3, 100000)


class ReaderStateTests(unittest.TestCase):
    setUp = fixture.PersonalCollectionTests.setUp
    def setUp(self):
        fixture.PersonalCollectionTests.setUp(self)
        self.digest = hashlib.sha256(BOOK).hexdigest()
        mock = patch.object(self.app, 'download_book', return_value=(BOOK, '')); mock.start(); self.addCleanup(mock.stop)
        self.mapping = AnchorMap(fb2_chapters(BOOK))

    def fetch(self, cursor=0):
        return self.app.reader_state(self.session, 'reader-a', '42', self.digest, cursor=cursor)

    def send(self, kind, **data):
        op = dict(account=self.fetch()['account'], op=secrets.token_hex(16), kind=kind, **data)
        for _ in range(2):
            self.assertEqual(self.app.reader_state(self.session, 'reader-a', '42', self.digest, op), {'ok': True})
        return op

    def test_two_way_progress_completion_and_retry(self):
        self.send('position', position=dict(record=3, byte=0), base=None, finished=False)
        reading = self.app.book('alice', '42')['reading']
        self.assertEqual(self.mapping.to_native(reading['chapter'], reading['anchor']), dict(record=3, byte=0))
        chapter, anchor = self.mapping.to_web(7, 5)
        self.app.save_state('alice', '42', dict(chapter=chapter, anchor=anchor, shelf='read'))
        remote = self.fetch()
        self.assertEqual(remote['position'], dict(record=7, byte=5)); self.assertTrue(remote['finished'])
        self.send('position', position=dict(record=8, byte=0), base=dict(record=3, byte=0), finished=False)
        self.assertEqual(len(self.fetch()['bookmarks']), 1)  # Both positions retained, retry did not duplicate.
        self.assertEqual(self.fetch()['bookmarks'][0]['position'], dict(record=7, byte=5))

    def test_web_and_device_bookmark_add_delete_and_labels(self):
        identity = secrets.token_hex(12)
        self.send('bookmark', id=identity, position=dict(record=3, byte=0), label='Прочитать ещё')
        marks = self.app.bookmarks('alice','42')['bookmarks']
        self.assertEqual(marks[0]['id'], identity)
        self.app.bookmarks('alice', '42', dict(action='rename', id=identity, label='Updated on web'))
        self.assertEqual(self.fetch()['bookmarks'][0]['label'], 'Updated on web')
        self.send('remove-bookmark', id=identity)
        self.assertFalse(self.fetch()['bookmarks'])

    def test_digest_account_and_ambiguous_operation_do_not_mutate(self):
        before=self.fetch()
        for digest, data in [('a'*64,None),(self.digest,dict(account='b'*64,op='c'*32,kind='position'))]:
            with self.assertRaises(WebError): self.app.reader_state(self.session,'reader-a','42',digest,data)
        self.assertEqual(before,self.fetch())
        op=self.send('position',position=dict(record=3,byte=0),base=None,finished=False)
        with self.assertRaises(WebError): self.app.reader_state(self.session,'reader-a','42',self.digest,op|{'finished':True})

    def test_bounded_pages_and_no_silent_loss_of_unmapped_old_state(self):
        for i in range(15):
            self.send('bookmark',id=secrets.token_hex(12),position=dict(record=7,byte=i),label='я'*120)
        cursor=0;marks=[];revisions=set()
        while True:
            result=self.fetch(cursor);self.assertLessEqual(len(wire(result).encode()),1900)
            marks.extend(result['bookmarks']);revisions.add(result['revision']);cursor=result['next']
            if not cursor:break
        self.assertEqual(len(marks),15);self.assertEqual(len(revisions),1)
        self.app.save_state('alice','42',dict(chapter=1,offset=.4))
        with self.assertRaises(WebError):self.fetch()
