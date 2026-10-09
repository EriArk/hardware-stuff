import json
import time
from unittest.mock import patch

from test_companion import ADMIN, CompanionHTTPCase
from octofox_library.desktop_discovery import DesktopDiscovery, local_address


class DesktopDiscoveryTests(CompanionHTTPCase):
    def packet(self, action='connect', **changes):
        return json.dumps({'product':'octofox-library', 'protocol':1, 'nonce':'a'*32,
                           'action':action, 'origin':self.base, **changes}).encode()

    def ticket(self):
        return json.loads(self.app.discovery.answer(self.packet(), '127.0.0.1'))['ticket']

    def test_local_handshake_creates_owner_without_manual_setup_key(self):
        headers = {'X-OctoFox-Native':self.ticket()}
        code, status, _ = self.call('/companion-api/status', headers=headers)
        self.assertEqual(code, 200); self.assertTrue(status['desktopSetup'])
        self.assertFalse(status['configured']); self.assertEqual(status['product'], 'octofox-library')
        code, result, _ = self.call('/companion-api/setup', ADMIN, headers)
        self.assertEqual(code, 200); self.assertTrue(result['readerReady'])
        self.assertEqual(self.call('/companion-api/setup', ADMIN, headers)[0], 409)
        self.assertFalse(self.call('/companion-api/status')[1]['desktopSetup'])

    def test_public_proxy_cannot_use_lan_setup_ticket(self):
        for header in ('X-Forwarded-For', 'Forwarded', 'CF-Connecting-IP', 'X-Forwarded-Proto', 'X-Real-IP'):
            code, _, _ = self.call('/companion-api/setup', ADMIN,
                                  {'X-OctoFox-Native':self.ticket(), header:'127.0.0.1'})
            self.assertEqual(code, 403, header)
        self.assertFalse(self.state['users'])

    def test_ticket_is_bound_to_peer_origin_and_lifetime(self):
        ticket = self.ticket(); headers={'X-OctoFox-Native':ticket, 'Host':self.base.removeprefix('http://')}
        self.assertEqual(self.app.discovery.request_origin(headers, '127.0.0.1'), self.base)
        self.assertIsNone(self.app.discovery.request_origin(headers, '192.168.1.20'))
        self.assertIsNone(self.app.discovery.request_origin(headers | {'Origin':'https://attacker.example'}, '127.0.0.1'))
        self.assertIsNone(self.app.discovery.request_origin(headers | {'Host':'attacker.example'}, '127.0.0.1'))
        with patch('octofox_library.desktop_discovery.time.monotonic', return_value=time.monotonic()+3601):
            self.assertIsNone(self.app.discovery.request_origin(headers, '127.0.0.1'))
        self.assertEqual(DesktopDiscovery(self.app).identity, self.app.discovery.identity)

    def test_native_origin_is_ephemeral_and_does_not_grant_account_access(self):
        other = self.base.replace('127.0.0.1','127.0.0.2')
        ticket=json.loads(self.app.discovery.answer(self.packet(origin=other), '127.0.0.1'))['ticket']
        headers={'Host':other.removeprefix('http://'),'Origin':other,'X-OctoFox-Native':ticket}
        self.assertEqual(self.call('/companion-api/status',headers=headers)[0],200)
        self.assertEqual(self.call('/companion-api/users',headers=headers)[0],401)
        self.assertNotIn(other,self.app.network.allowed())
        self.assertEqual(self.call('/companion-api/status',headers=headers | {'X-OctoFox-Native':'wrong'})[0],403)
        self.assertEqual(self.call('/companion-api/setup',ADMIN,headers)[0],200)
        self.assertIn(other,self.app.network.allowed())
        # A phone's normal browser can now open the same LAN address, but cannot
        # repeat setup or gain the owner's session through the discovery ticket.
        self.assertEqual(self.call('/companion-api/status',headers=headers | {'X-OctoFox-Native':''})[0],200)

    def test_discovery_rejects_untrusted_packets_and_caps_rate(self):
        for address in ('8.8.8.8','0.0.0.0','224.0.0.1','100.64.0.1','::1'):
            self.assertFalse(local_address(address));self.assertIsNone(self.app.discovery.answer(self.packet(),address))
        for packet in (b'{}',b'[]',b'x'*1025,self.packet(nonce='bad'),self.packet(origin='https://example.org'),self.packet(origin='http://user@127.0.0.1:8080')):
            self.assertIsNone(self.app.discovery.answer(packet,'127.0.0.1'))
        for _ in range(45): last=self.app.discovery.answer(self.packet('discover'),'192.168.1.30')
        self.assertIsNone(last)
