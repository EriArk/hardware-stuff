from http.cookiejar import CookieJar
import json
from urllib.request import build_opener, HTTPCookieProcessor, Request

from test_companion import ADMIN, READER, CompanionHTTPCase, PAYLOAD
from octofox_library.companion_accounts import AccountControls


class AccountControlTests(CompanionHTTPCase):
    def reader(self):
        self.setup_admin()
        self.assertEqual(self.call('/companion-api/users', READER)[0], 201)
        client = build_opener(HTTPCookieProcessor(CookieJar()))
        status, result, _ = self.call('/reader-api/login', READER, client=client)
        self.assertEqual(status, 200)
        return client, result['csrf']

    def test_companion_sign_in_also_signs_into_own_library_and_logout_closes_both(self):
        _, headers = self.setup_admin()
        self.assertEqual(len(headers.get_all('Set-Cookie')), 2)
        code, result, _ = self.call('/reader-api/me')
        self.assertEqual(code, 200)
        self.assertEqual(result['username'], ADMIN['username'])
        self.assertEqual(self.call('/companion-api/logout', {})[0], 200)
        self.assertEqual(self.call('/reader-api/me')[0], 401)

    def test_profile_changes_preserve_role_and_identity(self):
        self.reader()
        code, result, _ = self.call('/companion-api/users/2/profile', {
            'name': '<New name>', 'email': 'new@example.org', 'username': 'admin',
            'permissions': {'admin': True}, 'assignedLibraries': [1]})
        self.assertEqual(code, 200)
        self.assertEqual(result['user']['username'], 'reader')
        self.assertFalse(result['user']['admin'])
        self.assertEqual(self.state['users']['reader']['name'], '<New name>')
        self.assertEqual(self.call('/companion-api/users/999/profile', {})[0], 404)
        self.assertEqual(self.call('/companion-api/users/2/profile', {'name': 'A', 'email': 'bad'})[0], 400)

    def test_disabled_access_revokes_sessions_and_cache_but_retains_books_and_survives_restart(self):
        client, csrf = self.reader()
        with client.open(Request(self.base + '/reader-api/uploads', PAYLOAD,
                         {'Content-Type':'application/octet-stream', 'Origin':self.base,
                          'X-CSRF-Token':csrf, 'X-Upload-Filename':'kept.fb2'})) as response:
            book = json.load(response)
        code, result, _ = self.call('/companion-api/users/2/access', {'enabled': False})
        self.assertEqual(code, 200); self.assertFalse(result['user']['accessEnabled'])
        self.assertEqual(self.call('/reader-api/me', client=client)[0], 401)
        self.assertEqual(self.call('/reader-api/login', READER, client=client)[0], 403)
        self.app.companion.accounts = AccountControls(self.app.companion)
        self.assertEqual(self.call('/reader-api/login', READER, client=client)[0], 403)
        self.assertEqual(self.call('/companion-api/users/2/access', {'enabled': True})[0], 200)
        self.assertEqual(self.call('/reader-api/login', READER, client=client)[0], 200)
        self.assertEqual(self.call('/reader-api/books/' + str(book['id']), client=client)[0], 200)
        self.assertEqual(self.call('/companion-api/users/1/access', {'enabled': False})[0], 400)
        self.assertEqual(self.call('/companion-api/users/2/access', {'enabled': 'false'})[0], 400)

    def test_password_rotation_replaces_only_owned_matching_opds_and_rejects_old_password(self):
        client, _ = self.reader()
        self.state['opds']['another-device'] = ('unchanged-pass', 'reader')
        self.state['sorts']['reader'] = 'TITLE'
        code, result, _ = self.call('/companion-api/users/2/password', {'password': 'new-reader-123'})
        self.assertEqual(code, 200); self.assertTrue(result['ok'])
        self.assertEqual(self.state['opds']['reader'], ('new-reader-123', 'reader'))
        self.assertEqual(self.state['sorts']['reader'], 'TITLE')
        self.assertEqual(self.state['opds']['another-device'][0], 'unchanged-pass')
        self.assertEqual(self.call('/reader-api/me', client=client)[0], 401)
        self.assertEqual(self.call('/reader-api/login', READER, client=client)[0], 401)
        self.assertEqual(self.call('/reader-api/login', READER | {'password':'new-reader-123'}, client=client)[0], 200)
        self.assertNotIn('new-reader-123', json.dumps(self.call('/companion-api/users')[1]))

    def test_partial_password_change_is_durable_fail_closed_and_explicitly_recoverable(self):
        client, _ = self.reader(); self.state['fail_opds'] = True
        code, result, _ = self.call('/companion-api/users/2/password', {'password':'replacement-123'})
        self.assertEqual(code, 200); self.assertFalse(result['ok']); self.assertTrue(result['passwordPending'])
        self.app.companion.accounts = AccountControls(self.app.companion)
        self.assertEqual(self.call('/reader-api/login', READER, client=client)[0], 403)
        self.assertEqual(self.call('/reader-api/login', READER | {'password':'replacement-123'}, client=client)[0], 403)
        self.assertEqual(self.call('/companion-api/users/2/access', {'enabled':True})[0], 409)
        self.assertTrue(self.call('/companion-api/users/2/password', {'password':'replacement-123'})[1]['ok'])
        self.assertEqual(self.call('/reader-api/login', READER | {'password':'replacement-123'}, client=client)[0], 200)

    def test_self_password_change_revokes_admin_and_reader_sessions(self):
        self.setup_admin()
        _, result, headers = self.call('/companion-api/users/1/password', {'password':'new-admin-123'})
        self.assertTrue(all('Max-Age=0' in c for c in headers.get_all('Set-Cookie')))
        self.assertTrue(result['signedOut'])
        self.assertEqual(self.call('/companion-api/me')[0], 401)
        self.assertEqual(self.call('/reader-api/me')[0], 401)
        self.assertEqual(self.call('/companion-api/login', ADMIN | {'password':'new-admin-123'})[0], 200)

    def test_controls_require_admin_and_csrf_and_reject_invalid_password_without_mutation(self):
        client, _ = self.reader()
        for action, data in [('profile', READER), ('password', {'password':'new-reader-123'}), ('access', {'enabled':False})]:
            self.assertEqual(self.call('/companion-api/users/2/' + action, data, client=client)[0], 401)
            self.assertEqual(self.call('/companion-api/users/2/' + action, data, headers={'X-CSRF-Token':'wrong'})[0], 403)
        for value in ('short', 'я' * 37, None, 1):
            self.assertEqual(self.call('/companion-api/users/2/password', {'password':value})[0], 400)
        self.assertFalse(self.app.companion.accounts.state('reader')['password_pending'])
