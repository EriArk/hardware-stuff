"""HTTP lifecycle and failure recovery with a BookLore 2.3.1 contract fixture."""
import base64
from http.cookiejar import CookieJar
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, HTTPCookieProcessor, build_opener

from octofox_library.books_web import LibraryWeb, WebServer
from octofox_library.booklore_api import BookLoreAPI
from octofox_library.companion import credentials
from octofox_library.web_errors import WebError
from test_web_device_contents import PAYLOAD

ADMIN = dict(username='admin', password='test-pass-123', name='Administrator', email='admin@example.org')
READER = dict(username='reader', password='reader-pass-123', name='<Reader>', email='reader@example.org')


class BookLoreFixture(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def send(self, status, data):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.handle_api()

    def do_POST(self):
        self.handle_api()

    def do_PUT(self):
        self.handle_api()

    def handle_api(self):
        state = self.server.state
        data = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or '{}')
        path = self.path
        post = self.command == 'POST'
        auth = self.headers.get('Authorization', '')
        if state.get('redirect'):
            self.send_response(302)
            self.send_header('Location', state['redirect'])
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        username = auth.removeprefix('Bearer token-')
        user = state['users'].get(username)
        if path.startswith('/api/v1/opds'):
            valid = any(auth == 'Basic ' + base64.b64encode(f'{u}:{p}'.encode()).decode()
                        for u, (p, _) in state['opds'].items())
            body = b'<feed xmlns="http://www.w3.org/2005/Atom" />'
            self.send_response((200 if state['opds_enabled'] else 403) if valid else 401)
            self.send_header('Content-Type', 'application/atom+xml')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers(); self.wfile.write(body)
            return
        if path == '/api/v1/setup/status':
            return self.send(200, {'data': bool(state['users'])})
        if path == '/api/v1/setup' and post:
            if state['users']:
                return self.send(403, {})
            state['users'][data['username']] = data | {'id': 1, 'permissions': {'admin': True}}
            return self.send(200, {})
        if path == '/api/v1/auth/login' and post:
            target = state['users'].get(data['username'])
            if not target or target['password'] != data['password']:
                return self.send(401, {})
            state['logins'].append(data['username'])
            return self.send(200, {'accessToken': 'token-' + data['username'], 'refreshToken': 'refresh-' + data['username']})
        if path == '/api/v1/auth/refresh' and post:
            state['refreshes'] += 1
            return self.send(200, {'accessToken': 'token-admin', 'refreshToken': 'refresh-admin'})
        if not user:
            return self.send(401, {})
        if path == '/api/v1/users/me':
            return self.send(200, user)
        if path == '/api/v1/settings':
            if self.command == 'PUT':
                if not user['permissions']['admin']:
                    return self.send(403, {})
                if data != [{'name': 'OPDS_SERVER_ENABLED', 'value': True}]:
                    return self.send(400, {})
                state['opds_enabled'] = True
            return self.send(200, {'opdsServerEnabled': state['opds_enabled']})
        if path == '/api/v2/opds-users':
            if not post:
                return self.send(200, [{'username': u} for u, (_, owner) in state['opds'].items() if owner == username])
            if state['fail_opds']:
                state['fail_opds'] = False
                return self.send(503, {'private': 'never relay me'})
            if data['username'] in state['opds']:
                return self.send(409, {})
            state['opds'][data['username']] = (data['password'], username)
            return self.send(200, {})
        if not user['permissions']['admin']:
            return self.send(403, {})
        if path == '/api/v1/users':
            return self.send(200, list(state['users'].values()))
        if path == '/api/v1/auth/register':
            if data['username'] in state['users']:
                return self.send(409, {})
            state['users'][data['username']] = data | {'id': len(state['users']) + 1, 'permissions': {'admin': data.get('permissionAdmin', False)}}
            return self.send(204, None)
        return self.send(404, {})


class CompanionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        upstream = ThreadingHTTPServer(('127.0.0.1', 0), BookLoreFixture)
        upstream.state = self.state = {'users': {}, 'opds': {}, 'fail_opds': False, 'logins': [], 'refreshes': 0, 'opds_enabled': False}
        self.start(upstream)
        self.app = LibraryWeb(Path(self.temp.name) / 'library.sqlite3',
                              f'http://127.0.0.1:{upstream.server_port}/api/v1/opds', 'http://localhost')
        web = WebServer(('127.0.0.1', 0), self.app)
        self.base = self.app.origin = f'http://127.0.0.1:{web.server_port}'
        self.start(web)
        self.client = build_opener(HTTPCookieProcessor(CookieJar()))
        self.csrf = ''

    def start(self, server):
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)

    def call(self, path, data=None, headers=None, client=None):
        headers = {'Origin': self.base, 'Content-Type': 'application/json', 'X-CSRF-Token': self.csrf, **(headers or {})}
        try:
            response = (client or self.client).open(Request(self.base + path, None if data is None else json.dumps(data).encode(), headers), timeout=5)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response), response.headers

    def setup_admin(self):
        key = self.app.companion.key_path.read_text().strip()
        code, result, headers = self.call('/companion-api/setup', ADMIN, {'X-Setup-Key': key})
        self.assertEqual(code, 200, result); self.assertTrue(result['readerReady'])
        self.csrf = result['csrf']
        return result, headers

    def test_setup_login_create_reader_and_private_books(self):
        self.assertEqual(self.call('/companion-api/status')[1], {'configured': False})
        result, headers = self.setup_admin()
        self.assertTrue(result['user']['admin'])
        self.assertTrue(self.state['opds_enabled'])
        self.assertEqual(self.state['logins'], ['admin'])  # Avoid upstream duplicate JWTs within one second.
        self.assertNotIn(ADMIN['password'], json.dumps(result))
        self.assertIn('HttpOnly; SameSite=Strict; Path=/companion-api', headers['Set-Cookie'])
        code, result, _ = self.call('/companion-api/users', READER | {'permissionAdmin': True})
        self.assertEqual(code, 201); self.assertTrue(result['readerReady'])
        self.assertEqual(self.state['opds']['reader'][1], 'reader')
        self.assertFalse(self.state['users']['reader']['permissions']['admin'])
        code, result, _ = self.call('/companion-api/users')
        self.assertEqual(code, 200); self.assertNotIn('password', json.dumps(result))
        client = build_opener(HTTPCookieProcessor(CookieJar()))
        code, result, _ = self.call('/reader-api/login', READER, client=client)
        self.assertEqual(code, 200)
        with client.open(Request(self.base + '/reader-api/uploads', PAYLOAD,
                                {'Content-Type': 'application/octet-stream', 'Origin': self.base,
                                 'X-CSRF-Token': result['csrf'], 'X-Upload-Filename': 'sample.fb2'})) as response:
            book = json.load(response)
        # The reader account has no Companion access; its own reading remains available.
        self.assertEqual(self.call('/companion-api/login', READER, client=client)[0], 403)
        self.assertEqual(self.call('/reader-api/books/' + str(book['id']), client=client)[0], 200)
        other = build_opener(HTTPCookieProcessor(CookieJar()))
        self.assertEqual(self.call('/reader-api/login', ADMIN, client=other)[0], 200)
        self.assertEqual(self.call('/reader-api/books/' + str(book['id']), client=other)[0], 404)

    def test_setup_claim_origin_host_and_repeat_are_guarded(self):
        key = self.app.companion.key_path.read_text().strip()
        for headers in ({}, {'X-Setup-Key': 'wrong'}, {'X-Setup-Key': key, 'Origin': 'https://evil.example'},
                        {'X-Setup-Key': key, 'Origin': ''}, {'X-Setup-Key': key, 'Host': 'evil.example'}):
            self.assertEqual(self.call('/companion-api/setup', ADMIN, headers)[0], 403)
        self.assertFalse(self.state['users'])
        self.setup_admin()
        self.assertEqual(self.call('/companion-api/setup', ADMIN, {'X-Setup-Key': key})[0], 409)
        self.assertEqual(len(self.state['users']), 1)
        self.assertEqual(self.call('/companion-api/users', READER, {'X-CSRF-Token': 'wrong'})[0], 403)
        self.assertNotIn('reader', self.state['users'])

    def test_partial_create_can_be_completed_without_duplicate_or_password_reset(self):
        self.setup_admin(); self.state['fail_opds'] = True
        code, result, _ = self.call('/companion-api/users', READER)
        self.assertEqual(code, 201); self.assertFalse(result['readerReady'])
        self.assertNotIn('never relay', json.dumps(result))
        self.assertEqual(self.call('/companion-api/users', READER)[0], 409)
        self.assertEqual(self.call('/companion-api/reader-access', READER)[0], 200)
        self.assertEqual(len(self.state['users']), 2)
        self.assertEqual(self.call('/companion-api/reader-access', READER)[0], 200)
        self.state['opds']['reader'] = ('different-password', 'reader')
        self.assertEqual(self.call('/companion-api/reader-access', READER)[0], 409)
        self.assertEqual(self.state['opds']['reader'][0], 'different-password')

    def test_opds_name_owned_by_someone_else_is_not_adopted(self):
        self.setup_admin(); self.state['opds']['reader'] = (READER['password'], 'admin')
        code, result, _ = self.call('/companion-api/users', READER)
        self.assertEqual(code, 201); self.assertFalse(result['readerReady'])
        self.assertEqual(self.state['opds']['reader'][1], 'admin')

    def test_logout_expiry_privilege_revocation_and_refresh(self):
        self.setup_admin()
        session = next(iter(self.app.companion.sessions.values()))
        session.access = 'expired'
        self.assertEqual(self.call('/companion-api/me')[0], 200)
        self.assertEqual(self.state['refreshes'], 1)
        self.state['users']['admin']['permissions']['admin'] = False
        self.assertEqual(self.call('/companion-api/users', READER)[0], 403)
        self.assertEqual(self.call('/companion-api/logout', {})[0], 200)
        self.assertEqual(self.call('/companion-api/me')[0], 401)
        self.state['users']['admin']['permissions']['admin'] = True
        code, result, _ = self.call('/companion-api/login', ADMIN); self.assertEqual(code, 200)
        session = next(iter(self.app.companion.sessions.values())); session.expires = time.time() - 1
        self.assertEqual(self.call('/companion-api/me')[0], 401)

    def test_input_validation_and_setup_key_stays_private(self):
        for patch in ({'password': 'short'}, {'password': 'я' * 37}, {'username': 'a:b'}, {'username': []},
                      {'email': 'bad'}, {'name': 'a\nb'}):
            with self.assertRaises(WebError): credentials(ADMIN | patch, new=True)
        before = self.app.companion.key_path.read_bytes()
        self.assertNotIn(before.strip().decode(), json.dumps(self.call('/companion-api/status')[1]))
        self.setup_admin()
        self.assertEqual(self.app.companion.key_path.read_bytes(), before)
        for path in ('/companion-setup-key', '/companion-api/setup-key'):
            self.assertIn(self.call(path)[0], (401, 404))

    def test_upstream_redirect_is_rejected_without_forwarding_credentials(self):
        self.setup_admin()
        self.state['redirect'] = self.base + '/companion-api/status'
        api = self.app.companion.api
        self.assertEqual(BookLoreAPI(api.base + '/api/v1/opds/').base, api.base)
        with self.assertRaises(WebError) as caught:
            api.request('/api/v1/users/me', token='must-not-leave-upstream')
        self.assertEqual(caught.exception.status, 502)


if __name__ == '__main__':
    unittest.main()
