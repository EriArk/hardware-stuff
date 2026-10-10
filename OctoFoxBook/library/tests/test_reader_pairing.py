import base64
import hashlib
import unittest
from unittest.mock import patch
import test_web_personal_collections as fixture
from octofox_library.web_errors import WebError


class ReaderPairingTests(unittest.TestCase):
    setUp=fixture.PersonalCollectionTests.setUp
    def test_target_account_key_only_and_scoped_revocation(self):
        pairing=self.app.companion.pairing
        def request(path,data=None,**kwargs):
            if path=='/api/v1/users/me': return {'username':'alice'}
            if path=='/api/v2/opds-users':
                self.assertNotEqual(data['password'],'account-password');self.assertTrue(data['username'].startswith('octodev_'))
                return {'id':42}
            raise AssertionError(path)
        with patch.object(self.app.network,'allowed',return_value=['https://books.example']), \
             patch.object(self.app.companion,'enable_reader_service'), \
             patch.object(self.app.companion.api,'login',return_value={'accessToken':'target-user-token'}), \
             patch.object(self.app.companion.api,'request',side_effect=request), \
             patch.object(self.app.client,'fetch_xml'):
            result=pairing.create(dict(username='alice',password='account-password',device='reader-001122334455',origin='https://books.example'),'admin-token')
            self.assertEqual(result['url'],'https://books.example/reader-api/device')
            auth='Basic '+base64.b64encode((result['username']+':'+result['key']).encode()).decode()
            self.assertEqual(self.app.authenticate(auth,device_allowed=True,device=result['device']),'alice')
            for kwargs in ({},{'device_allowed':True,'device':'reader-other'}):
                with self.assertRaises(WebError):self.app.authenticate(auth,**kwargs)
            with self.app.db() as db:
                row=dict(db.execute('SELECT * FROM reader_keys').fetchone())
            self.assertEqual(row['digest'],hashlib.sha256(result['key'].encode()).hexdigest())
            self.assertNotIn(result['key'],str(row));self.assertNotIn('account-password',str(row))
            pairing.revoke(result['username'])
            with self.assertRaises(WebError):self.app.authenticate(auth,device_allowed=True,device=result['device'])

    def test_unconfigured_or_http_origin_rejected_before_account_access(self):
        with patch.object(self.app.companion.api,'login') as login:
            for origin in ('http://books.example','https://attacker.example'):
                with self.assertRaises(WebError):self.app.companion.pairing.create(dict(username='alice',password='p',device='reader-001122334455',origin=origin),'admin')
            login.assert_not_called()

    def test_another_account_uses_its_own_token_and_owner_needs_no_password(self):
        def request(path,data=None,**kwargs):
            if path=='/api/v1/users/me':return {'username':'alice' if kwargs['token']=='target' else 'owner'}
            if path=='/api/v2/opds-users':
                self.assertEqual(kwargs['token'],'target');return {'id':8}
            raise AssertionError(path)
        with patch.object(self.app.network,'allowed',return_value=['https://books.example']), \
             patch.object(self.app.companion,'enable_reader_service'), \
             patch.object(self.app.companion.api,'login',return_value={'accessToken':'target'}) as login, \
             patch.object(self.app.companion.api,'request',side_effect=request), \
             patch.object(self.app.client,'fetch_xml'):
            key=self.app.companion.pairing.create(dict(username='alice',password='target-password',device='reader-001122334455',origin='https://books.example'),'admin')
            login.assert_called_once_with('alice','target-password');self.assertEqual(key['account'],'alice')
