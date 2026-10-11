import io
import json
import socket
import tempfile
import threading
import unittest
import urllib.request
import urllib.error
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

from octofox_library.books_web import LibraryWeb, Session, WebServer
from octofox_library.external_opds import Fetcher, parse_feed, valid_url, public_addresses, search_url
from octofox_library.web_external import ExternalCatalogs
from octofox_library.web_errors import WebError
from test_web_device_contents import PAYLOAD

ROOT = 'https://catalog.example/opds'


def feed(number=1, download='/book.fb2', fmt='application/x-fictionbook+xml'):
    return f'''<feed xmlns="http://www.w3.org/2005/Atom"><title>Test catalog</title>
      <link rel="next" href="?page=2"/><entry><id>book-{number}</id><title>Book {number}</title>
      <author><name>Test Author</name></author>
      <link rel="http://opds-spec.org/acquisition/open-access" type="{fmt}" href="{download}"/>
      </entry></feed>'''.encode()


class ExternalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.app = LibraryWeb(Path(self.temp.name) / 'state.sqlite', 'http://127.0.0.1:1/api/v1/opds', 'http://localhost')
        self.ext = self.app.external
        self.fetch = patch.object(self.ext.fetcher, 'fetch', side_effect=self.remote).start()
        self.addCleanup(patch.stopall)
        self.source = self.ext.save('alice', {'name':'Test','url':ROOT, 'username':'user','password':'private'})['id']
        self.ext.browse('alice', self.source)
        self.item = self.ext.browse('alice', self.source)['items'][0]['id']

    def remote(self, url, **kwargs):
        if 'book.fb2' in url:
            if kwargs.get('progress'):
                kwargs['progress'](10, len(PAYLOAD))
                kwargs['progress'](len(PAYLOAD), len(PAYLOAD))
            return PAYLOAD, url, 'application/x-fictionbook+xml'
        return feed(2 if 'page=2' in url else 1), url, 'application/atom+xml'

    def test_cross_page_selection_persistent_queue_and_content_dedup(self):
        first = self.ext.enqueue('alice', self.source, self.item)['id']
        page2 = self.ext.browse('alice', self.source, ROOT + '?page=2')
        second = self.ext.enqueue('alice', self.source, page2['items'][0]['id'])['id']
        self.assertNotEqual(first, second)
        self.assertEqual(first, self.ext.enqueue('alice', self.source, self.item)['id'])
        restored = ExternalCatalogs(self.app)
        restored.fetcher = self.ext.fetcher
        self.assertEqual(len(restored.jobs('alice')['downloads']), 2)
        self.assertTrue(restored.run_one())
        self.assertTrue(restored.run_one())
        jobs = restored.jobs('alice')['downloads']
        self.assertEqual({j['state'] for j in jobs}, {'complete'})
        self.assertEqual(len({j['book'] for j in jobs}), 1)
        with self.app.db() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM personal_library WHERE owner='alice'").fetchone()[0], 1)
        self.assertTrue(self.app.upload_path('alice', jobs[0]['book']).is_file())

    def test_account_isolation_and_credentials_redacted(self):
        self.ext.enqueue('alice', self.source, self.item)
        self.assertEqual(self.ext.jobs('bob')['downloads'], [])
        self.assertEqual(self.ext.catalogs('bob')['catalogs'], [])
        for action in (lambda: self.ext.browse('bob', self.source),
                       lambda: self.ext.enqueue('bob', self.source, self.item),
                       lambda: self.ext.remove('bob', self.source)):
            with self.assertRaises(WebError): action()
        serialized = json.dumps(self.ext.catalogs('alice')) + json.dumps(self.ext.jobs('alice'))
        self.assertNotIn('private', serialized)
        self.assertNotIn('authorization', serialized)

    def test_cancel_retry_failure_and_catalog_disconnect(self):
        job = self.ext.enqueue('alice', self.source, self.item)['id']
        self.ext.action('alice', job, 'cancel')
        self.assertFalse(self.ext.run_one())
        self.ext.action('alice', job, 'retry')
        with patch.object(self.app, 'upload_book', side_effect=ValueError('private URL/password')):
            self.ext.run_one()
        result = self.ext.jobs('alice')['downloads'][0]
        self.assertEqual(result['state'], 'failed')
        self.assertNotIn('private', result['error'])
        self.ext.action('alice', job, 'retry')
        self.ext.run_one()
        self.ext.remove('alice', self.source)
        self.assertEqual(self.ext.jobs('alice')['downloads'][0]['state'], 'complete')
        self.assertTrue(self.app.upload_path('alice', self.ext.jobs('alice')['downloads'][0]['book']))

    def test_cancel_during_transfer_prevents_import(self):
        job = self.ext.enqueue('alice', self.source, self.item)['id']
        def transfer(url, **kwargs):
            self.ext.action('alice', job, 'cancel')
            kwargs['progress'](10, 100)
        self.fetch.side_effect = transfer
        with patch.object(self.app, 'upload_book') as upload:
            self.ext.run_one()
            upload.assert_not_called()
        self.assertEqual(self.ext.jobs('alice')['downloads'][0]['state'], 'cancelled')

    def test_worker_restarts_interrupted_job(self):
        job = self.ext.enqueue('alice', self.source, self.item)['id']
        with self.app.db() as db:
            db.execute("UPDATE external_downloads SET state='importing' WHERE id=?", (job,))
        # Simulate a crash after upload committed but before marking the job complete.
        self.app.upload_book('alice', PAYLOAD)
        with patch.object(self.ext, 'run'):
            self.ext.start(); self.ext.stop()
        self.ext.stop_event.clear()
        self.ext.run_one()
        self.assertEqual(self.ext.jobs('alice')['downloads'][0]['state'], 'complete')
        with self.app.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM uploaded_books').fetchone()[0], 1)

    def test_partial_entry_and_fb2_zip(self):
        packed = io.BytesIO()
        with zipfile.ZipFile(packed, 'w') as z: z.writestr('book.fb2', PAYLOAD)
        partial = b'''<feed xmlns="http://www.w3.org/2005/Atom"><entry><id>partial</id><title>Partial</title>
          <link rel="alternate" type="application/atom+xml;type=entry" href="/detail"/></entry></feed>'''
        def remote(url, **kwargs):
            data = packed.getvalue() if url.endswith('/file.zip') else feed(5, '/file.zip', 'application/fb2+zip') if url.endswith('/detail') else partial
            return data, url, ''
        self.fetch.side_effect = remote
        item = self.ext.browse('alice', self.source)['items'][0]
        self.assertTrue(item['available'])
        self.ext.enqueue('alice', self.source, item['id'])
        self.ext.run_one()
        self.assertEqual(self.ext.jobs('alice')['downloads'][0]['state'], 'complete')

    def test_http_auth_and_csrf(self):
        self.app.sessions['token'] = Session('alice', 'Basic eA==', 'csrf', 9999999999)
        server = WebServer(('127.0.0.1', 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        address = f'http://127.0.0.1:{server.server_port}/reader-api/external/downloads'
        with patch.object(self.app, 'authenticate', return_value='alice'):
            for headers, code in [({}, 401), ({'Cookie':'books_session=token'}, 403)]:
                req = urllib.request.Request(address, data=b'{}', headers=headers)
                with self.assertRaises(urllib.error.HTTPError) as error: urllib.request.urlopen(req)
                self.assertEqual(error.exception.code, code)
            req = urllib.request.Request(address, data=json.dumps({'catalog':self.source,'item':self.item}).encode(),
                headers={'Cookie':'books_session=token','X-CSRF-Token':'csrf','Content-Type':'application/json'})
            with urllib.request.urlopen(req) as response: self.assertEqual(response.status, 202)


class ParserTests(unittest.TestCase):
    def test_relative_links_pagination_and_no_paid_download(self):
        page = parse_feed(feed(), ROOT)
        self.assertEqual(page['next'], ROOT + '?page=2')
        self.assertEqual(page['items'][0]['downloads'][0]['url'], 'https://catalog.example/book.fb2')
        page = parse_feed(feed().replace(b'acquisition/open-access', b'acquisition/buy'), ROOT)
        self.assertEqual(page['items'][0]['downloads'], [])

    def test_entity_html_and_utf16_are_rejected(self):
        for data in [b'<html/>', b'<!DOCTYPE feed><feed/>', '<feed/>'.encode('utf-16')]:
            with self.assertRaises(WebError): parse_feed(data, ROOT)

    def test_search_template(self):
        data = b'<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/"><Url type="application/atom+xml" template="/search?q={searchTerms}&amp;lang={language?}"/></OpenSearchDescription>'
        self.assertEqual(search_url(data, ROOT, 'Alice & Bob'), 'https://catalog.example/search?q=Alice%20%26%20Bob&lang=')

    def test_gutenberg_search_uses_current_https_host(self):
        data = b'<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/"><Url type="application/atom+xml" template="http://m.gutenberg.org/ebooks/search.opds/?query={searchTerms}"/></OpenSearchDescription>'
        self.assertEqual(search_url(data, 'https://www.gutenberg.org/catalog/osd-books.xml', 'alice'),
                         'https://www.gutenberg.org/ebooks/search.opds/?query=alice')
        # Other providers do not gain an arbitrary origin rewrite.
        self.assertTrue(search_url(data, ROOT, 'alice').startswith('http://m.gutenberg.org/'))

    def test_address_validation(self):
        self.assertEqual(valid_url('https://example.com/caf\u00e9'), 'https://example.com/caf%C3%A9')
        for url in ['file:///etc/passwd', 'https://user:pass@example.com', 'https://example.com/\nfoo', 'https://example.com:bad', 'http://x\\@example.com']:
            with self.assertRaises(WebError): valid_url(url)
        for address in ['127.0.0.1', '10.0.0.1', '169.254.169.254', '::1', '::ffff:127.0.0.1', '224.0.0.1']:
            with patch('socket.getaddrinfo', return_value=[(socket.AF_INET, 1, 6, '', (address,80))]):
                with self.assertRaises(WebError): public_addresses('example.com', 80)

    def test_real_transport_redirect_credentials_and_limits(self):
        observed = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a): pass
            def do_GET(self):
                observed.append(self.headers.get('Authorization'))
                if self.path == '/redirect':
                    self.send_response(302)
                    self.send_header('Location', f'http://localhost:{self.server.server_port}/book')
                    self.end_headers(); return
                self.send_response(200); self.send_header('Content-Length', '5'); self.end_headers(); self.wfile.write(b'hello')
        server = ThreadingHTTPServer(('127.0.0.1',0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        url = f'http://127.0.0.1:{server.server_port}'
        fetcher = Fetcher(resolve=lambda host,port: socket.getaddrinfo('127.0.0.1', port, type=socket.SOCK_STREAM))
        data, _, _ = fetcher.fetch(url + '/redirect', root=url, authorization='Basic secret')
        self.assertEqual(data, b'hello'); self.assertEqual(observed, ['Basic secret', None])
        with self.assertRaises(WebError): fetcher.fetch(url + '/book', limit=4)


if __name__ == '__main__': unittest.main()
