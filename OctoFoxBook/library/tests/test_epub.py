import io
import json
import threading
import unittest
import zipfile
import xml.etree.ElementTree as ET
from email.message import Message
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from unittest.mock import patch

import test_books_web as helpers
from octofox_library.epub import epub_to_fb2, is_epub
from octofox_library.books_web import LibraryWeb, WebError, WebServer, fb2_chapters
from octofox_library.web_uploads import parse_upload

PNG = b'\x89PNG\r\n\x1a\n' + b'cover'


def epub(changes=None):
    entries = {
        'mimetype': 'application/epub+zip',
        'META-INF/container.xml': '<container><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>',
        'OPS/book.opf': '''<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
        <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
        <dc:title>Настоящее название</dc:title><dc:creator>Иван Автор</dc:creator>
        <dc:description>Описание &lt;b&gt;книги&lt;/b&gt;</dc:description>
        <dc:language>ru</dc:language><dc:subject>sf_fantasy</dc:subject>
        <meta name="calibre:series" content="Истории"/><meta name="calibre:series_index" content="2"/>
        </metadata><manifest>
        <item id="second" href="Text/2.xhtml" media-type="application/xhtml+xml"/>
        <item id="first" href="Text/1.xhtml" media-type="application/xhtml+xml"/>
        <item id="cover" href="Images/cover.png" media-type="image/png" properties="cover-image"/>
        </manifest><spine><itemref idref="first"/><itemref idref="second"/></spine></package>''',
        'OPS/Text/2.xhtml': '<html xmlns="http://www.w3.org/1999/xhtml"><body><h1>Вторая</h1><p>Конец.</p></body></html>',
        'OPS/Text/1.xhtml': '''<!DOCTYPE html><html xmlns="http://www.w3.org/1999/xhtml"><body>
        <h1>Первая</h1><div><p>Начало <em>истории</em>.</p><p>Дальше.</p></div>
        <script>alert('BAD_SCRIPT')</script><style>BAD_STYLE</style><p hidden="hidden">BAD_HIDDEN</p>
        <img src="../Images/cover.png"/></body></html>''',
        'OPS/Images/cover.png': PNG,
    }
    entries.update(changes or {})
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return out.getvalue()


class EpubConversionTest(unittest.TestCase):
    def test_metadata_cover_spine_order_and_sanitized_text(self):
        source = epub()
        self.assertTrue(is_epub(source))
        result = epub_to_fb2(source)
        self.assertIn(b'<FictionBook', result[:256])  # ESP32 signature probe
        record, cover = parse_upload(result)
        self.assertEqual(record['title'], 'Настоящее название')
        self.assertEqual(record['author'], 'Иван Автор')
        self.assertEqual(record['series'], 'Истории')
        self.assertEqual(record['seriesNumber'], '2')
        self.assertEqual(record['language'], 'ru')
        self.assertEqual(record['genres'], ['sf_fantasy'])
        self.assertIn('книги', record['summary'])
        self.assertNotIn('<b>', record['summary'])
        self.assertEqual(cover, PNG)
        self.assertEqual(len(ET.fromstring(result).findall('{*}binary')), 1)
        chapters = fb2_chapters(source)
        self.assertEqual([c['title'] for c in chapters], ['Первая', 'Вторая'])
        self.assertIn('<em>истории</em>', chapters[0]['html'])
        self.assertIn('Конец.', chapters[1]['html'])
        self.assertNotIn('BAD_', str(chapters))

    def test_epub2_cover_and_epub3_series(self):
        with zipfile.ZipFile(io.BytesIO(epub())) as archive:
            opf = archive.read('OPS/book.opf').decode()
        opf = opf.replace('properties="cover-image"', '').replace('</metadata>', '''
        <meta name="cover" content="cover"/>
        <meta property="belongs-to-collection" id="s">Цикл</meta>
        <meta refines="#s" property="collection-type">series</meta>
        <meta refines="#s" property="group-position">3</meta></metadata>''')
        record, cover = parse_upload(epub_to_fb2(epub({'OPS/book.opf': opf})))
        self.assertEqual((record['series'], record['seriesNumber'], cover), ('Цикл', '3', PNG))

    def test_rejects_unsafe_paths_entities_external_resources_drm_and_bombs(self):
        for changes in [
            {'../escape': 'bad'},
            {'META-INF/container.xml': '<!DOCTYPE x [<!ENTITY x "bad">]><container/>'},
            {'OPS/Text/1.xhtml': '<html><body><img src="https://example.org/x.png"/></body></html>'},
            {'META-INF/encryption.xml': '<encryption><EncryptedData><CipherReference URI="OPS/Text/1.xhtml"/></EncryptedData></encryption>'},
            {'bomb': '0' * 2000000},
            {'OPS/Text/1.xhtml': '<html><body>' + '<div>' * 100 + 'text' + '</div>' * 100 + '</body></html>'},
            {'OPS/Text/1.xhtml': '<html><body>broken'},
            {'mimetype': 'application/zip'},
        ]:
            with self.subTest(changes=list(changes)), self.assertRaises(ValueError):
                epub_to_fb2(epub(changes))
        with self.assertRaises(ValueError):
            epub_to_fb2(epub(), max_bytes=128)
        self.assertFalse(is_epub(b'PKnot-a-book'))


class EpubLibraryTest(unittest.TestCase):
    setUp = helpers.BooksWebTest.setUp
    seed = helpers.BooksWebTest.seed

    def test_upload_dedup_original_private_collection_and_device_copy(self):
        source = epub()
        book = self.app.upload_book('alice', source)
        identity = book['id']
        path = self.app.upload_path('alice', identity)
        self.assertEqual(path.with_suffix('.epub').read_bytes(), source)
        self.assertEqual(parse_upload(path.read_bytes())[0]['title'], 'Настоящее название')
        self.assertEqual(book['format'], 'epub')
        self.assertEqual(self.app.upload_book('alice', source)['id'], identity)
        with self.assertRaises(WebError):
            self.app.book('bob', identity)
        self.assertEqual(self.app.download_book(self.session, identity)[0], path.read_bytes())
        self.assertEqual(self.app.download_book(self.session, identity, original=True)[0], source)
        self.assertEqual(len(self.app.chapters(self.session, identity)), 2)
        folder = self.app.personal_collections('alice', {'action':'create', 'name':'EPUB'})['id']
        self.app.book_collections('alice', identity, {'id':folder, 'selected':True})
        self.assertEqual(self.app.catalog(self.session, {'personalCollection':[folder]})['total'], 1)
        self.app.enqueue('alice', identity, 'default')
        self.assertIn(f'/reader-api/uploads/{identity}/download'.encode(), self.app.poll('alice', 'reader-aabbcc', 2))
        reopened = LibraryWeb(self.path, 'http://127.0.0.1:1/api/v1/opds', 'http://127.0.0.1')
        self.assertEqual(reopened.download_book(self.session, identity)[0], path.read_bytes())

    def upstream(self):
        response = io.BytesIO(epub())
        response.headers = Message()
        response.headers['Content-Type'] = 'application/epub+zip'
        return response

    def test_shared_opds_epub_read_download_and_device_http(self):
        with patch.object(self.app.client, 'open', side_effect=lambda *a, **kw: self.upstream()):
            self.assertEqual(len(self.app.chapters(self.session, '42')), 2)
            data, name = self.app.download_book(self.session, '42')
            self.assertIn('.fb2', name)
            self.assertIn('FictionBook', data.decode())
            server = WebServer(('127.0.0.1', 0), self.app)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                base = f'http://127.0.0.1:{server.server_address[1]}'
                asset = base + '/reader-api/compatible/42/download'
                with self.assertRaises(HTTPError) as error:
                    urlopen(asset)
                self.assertEqual(error.exception.code, 401)
                with patch.object(self.app, 'authenticate', return_value='alice'):
                    with urlopen(Request(asset, headers={'Authorization':'Basic test'})) as response:
                        self.assertEqual(response.headers.get_content_type(), 'application/x-fictionbook+xml')
                        self.assertEqual(response.read(), data)
                with patch.object(self.app, 'authenticate', return_value='outsider'):
                    with self.assertRaises(HTTPError) as error:
                        urlopen(Request(asset, headers={'Authorization':'Basic other'}))
                    self.assertEqual(error.exception.code, 404)
            finally:
                server.shutdown(); server.server_close(); thread.join()


if __name__ == '__main__':
    unittest.main()
