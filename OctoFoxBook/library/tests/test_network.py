"""Real HTTP origin policy, persisted settings, and connection confirmation."""
import io
import base64
import time
import xml.etree.ElementTree as ET
from unittest.mock import patch, Mock

from octofox_library.network import NetworkSettings, origin
from octofox_library.qr import qr_data_url
from octofox_library.web_errors import WebError
from test_companion import CompanionHTTPCase, ADMIN


class NetworkTests(CompanionHTTPCase):
    def test_origin_validation_and_normalization(self):
        self.assertEqual(origin('https://BOOKS.example:443/'), 'https://books.example')
        self.assertEqual(origin('http://[::1]:8080'), 'http://[::1]:8080')
        for value in ('http://user:pass@host', 'https://host/path', 'https://host?x=1',
                      'https://host#frag', 'https://*.example.org', 'http://0.0.0.0',
                      'http://224.0.0.1', 'http://host:0', 'http://host:65536',
                      'http://host\nEVIL=x', 'http://host\\evil', 'http://127.0.0.1%2f',
                      'http://bad_host', 'http://-host', 'null', None, []):
            with self.subTest(value=value), self.assertRaises(WebError): origin(value)

    def test_admin_save_persists_and_rejects_stale_updates(self):
        self.setup_admin()
        self.assertEqual(self.call('/companion-api/network')[1]['revision'], 0)
        data = {'revision': 0, 'additionalOrigins': ['https://books.example.org']}
        code, saved, _ = self.call('/companion-api/network', data)
        self.assertEqual(code, 200); self.assertEqual(saved['revision'], 1)
        reopened = NetworkSettings(self.app)
        self.assertIn('https://books.example.org', reopened.allowed())
        self.assertEqual(self.call('/companion-api/network', data)[0], 409)
        self.assertEqual(reopened.snapshot()['revision'], 1)
        self.assertEqual(self.call('/companion-api/network', {'revision': 1, 'additionalOrigins': ['http://bad/path']})[0], 400)
        self.assertEqual(reopened.snapshot()['revision'], 1)

    def test_multiple_origins_secure_cookies_and_cross_origin_posts(self):
        self.setup_admin()
        self.app.network.save({'revision': 0, 'additionalOrigins': ['https://books.example.org']}, self.base)
        public = {'Host': 'books.example.org', 'Origin': 'https://books.example.org'}
        code, _, headers = self.call('/companion-api/login', ADMIN, public)
        self.assertEqual(code, 200); self.assertIn('; Secure', headers['Set-Cookie'])
        code, _, headers = self.call('/reader-api/login', ADMIN, public)
        self.assertEqual(code, 200); self.assertIn('; Secure', headers['Set-Cookie'])
        code, _, headers = self.call('/reader-api/login', ADMIN)
        self.assertEqual(code, 200); self.assertNotIn('; Secure', headers['Set-Cookie'])
        self.assertEqual(self.call('/reader-api/login', ADMIN, {'Origin': 'https://books.example.org'})[0], 403)
        self.assertEqual(self.call('/companion-api/login', ADMIN, {'Host': 'other.example'})[0], 403)
        self.assertEqual(self.call('/companion-api/login', ADMIN, {'X-Forwarded-Host': 'books.example.org', 'X-Forwarded-Proto': 'https'})[0], 200)
        self.app.network.save({'revision': 1, 'additionalOrigins': []}, self.base)
        self.assertEqual(self.call('/reader-api/login', ADMIN, public)[0], 403)

    def test_current_address_and_secure_scheme_cannot_be_lost(self):
        self.setup_admin()
        self.app.network.save({'revision': 0, 'additionalOrigins': ['https://books.example.org']}, self.base)
        with self.assertRaises(WebError) as error:
            self.app.network.save({'revision': 1, 'additionalOrigins': []}, 'https://books.example.org')
        self.assertEqual(error.exception.status, 409)
        with self.assertRaises(WebError):
            self.app.network.save({'revision': 1, 'additionalOrigins': ['https://books.example.org', 'http://books.example.org:8080']}, self.base)
        self.app.network.save({'revision': 1, 'additionalOrigins': ['http://books.example.org:8080']}, self.base)
        self.app.origin = 'https://books.example.org'
        self.assertEqual(self.app.network.allowed(), ['https://books.example.org'])

    def test_readers_and_cross_site_requests_cannot_change_settings(self):
        self.assertEqual(self.call('/companion-api/network')[0], 401)
        self.setup_admin()
        self.assertEqual(self.call('/companion-api/network', {'revision': 0, 'additionalOrigins': []}, {'X-CSRF-Token': 'wrong'})[0], 403)
        self.state['users']['admin']['permissions']['admin'] = False
        self.assertEqual(self.call('/companion-api/network')[0], 403)
        self.assertEqual(self.app.network.snapshot()['revision'], 0)

    def test_confirmation_requires_saved_origin_nonce_and_valid_origin_header(self):
        self.setup_admin()
        self.assertEqual(self.call('/companion-api/network/check', {'origin': 'https://unsaved.example'})[0], 400)
        self.app.network.save({'revision': 0, 'additionalOrigins': ['https://books.example.org']}, self.base)
        code, check, _ = self.call('/companion-api/network/check', {'origin': 'https://books.example.org'})
        self.assertEqual(code, 200); self.assertIn('#', check['url'])
        self.assertEqual(check['qrDataUrl'], qr_data_url(check['url']))
        self.assertEqual(check['expiresIn'], 600)
        token = {'token': check['token']}
        self.assertEqual(self.call('/companion-api/network/confirm', token)[0], 404)
        public = {'Host': 'books.example.org', 'Origin': 'https://books.example.org'}
        self.assertEqual(self.call('/companion-api/network/confirm', token, public | {'Origin': ''})[0], 403)
        self.assertEqual(self.call('/companion-api/network/confirm', {'token': 'wrong'}, public)[0], 404)
        self.assertEqual(self.call('/companion-api/network/confirm', token, public)[0], 200)
        result = self.call('/companion-api/network/check-status', token)[1]
        self.assertIsInstance(result['confirmedAt'], int)
        self.assertEqual(result['scope'], 'browser')
        self.assertEqual(result['externalReachability'], 'unverified')
        with self.assertRaises(WebError): self.app.network.check_status(token, 999)
        self.app.network.checks[check['token']]['expires'] = time.time() - 1
        self.assertEqual(self.call('/companion-api/network/confirm', token, public)[0], 404)

    def test_qr_is_self_contained_svg_without_interpolated_markup(self):
        data = qr_data_url('https://example.org/#<script>alert(1)</script>')
        self.assertTrue(data.startswith('data:image/svg+xml;base64,'))
        svg = base64.b64decode(data.split(',', 1)[1])
        self.assertNotIn(b'<script', svg)
        self.assertNotIn(b'https://example', svg)
        root = ET.fromstring(svg)
        self.assertEqual([e.tag.rsplit('}', 1)[1] for e in root], ['rect', 'path'])
        self.assertEqual(root[0].get('fill'), '#fff')
        self.assertEqual(root[1].get('fill'), '#000')

    def test_removing_address_invalidates_connection_check(self):
        self.setup_admin()
        self.app.network.save({'revision': 0, 'additionalOrigins': ['https://books.example.org']}, self.base)
        token = self.app.network.begin_check({'origin': 'https://books.example.org'}, 1)['token']
        self.app.network.save({'revision': 1, 'additionalOrigins': []}, self.base)
        with self.assertRaises(WebError): self.app.network.check_status({'token': token}, 1)

    def test_ip_lookup_is_fixed_bounded_cached_and_not_reachability(self):
        self.setup_admin()
        opener = Mock()
        opener.open.return_value = io.BytesIO(b'{"ip":"8.8.8.8"}')
        with patch('octofox_library.network.build_opener', return_value=opener):
            code, result, _ = self.call('/companion-api/network/external-ip', {'url': 'http://private.invalid'})
            self.assertEqual(code, 200); self.assertEqual(result['address'], '8.8.8.8')
            self.assertEqual(result['externalReachability'], 'unverified')
            self.assertEqual(self.call('/companion-api/network/external-ip', {})[0], 200)
        self.assertEqual(opener.open.call_count, 1)
        request = opener.open.call_args.args[0]
        self.assertEqual(request.full_url, 'https://api.ipify.org?format=json')
        self.assertNotIn('Authorization', request.headers); self.assertNotIn('Cookie', request.headers)
        self.assertEqual(opener.open.call_args.kwargs['timeout'], 6)
        for body in (b'{"ip":"127.0.0.1"}', b'x' * 300, b'{}', b'not-json'):
            self.app.network.public_ip = None
            opener.open.return_value = io.BytesIO(body)
            with patch('octofox_library.network.build_opener', return_value=opener):
                self.assertEqual(self.call('/companion-api/network/external-ip', {})[0], 502)
        self.assertEqual(self.app.network.snapshot()['revision'], 0)
