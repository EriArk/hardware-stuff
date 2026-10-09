"""Bounded LAN discovery and an automatic native-app setup handshake.

The local network is the trust boundary for claiming an unconfigured server.
Tickets permit a specific direct origin; they never grant an account session.
"""
from collections import OrderedDict
import ipaddress
import json
import logging
import os
import re
import secrets
import socket
import threading
import time
import uuid
from urllib.parse import urlsplit

PRODUCT = 'octofox-library'
PORT = 49645
PRIVATE = tuple(ipaddress.ip_network(n) for n in ('127.0.0.0/8', '10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '169.254.0.0/16'))


def local_address(value):
    try:
        address = ipaddress.ip_address(value)
        if getattr(address, 'ipv4_mapped', None):
            address = address.ipv4_mapped
        return address.version == 4 and any(address in network for network in PRIVATE)
    except ValueError:
        return False


class DesktopDiscovery:
    def __init__(self, app):
        self.app = app
        self.lock = threading.Lock()
        self.tickets = OrderedDict()
        self.rate = OrderedDict()
        self.socket = None
        path = app.database.parent / 'instance-id'
        try:
            with path.open('x', encoding='ascii') as out:
                out.write(str(uuid.uuid4()))
        except FileExistsError:
            pass
        self.identity = str(uuid.UUID(path.read_text(encoding='ascii').strip()))

    def metadata(self):
        return {'product': PRODUCT, 'protocol': 1, 'instanceId': self.identity,
                'name': os.environ.get('OCTOFOX_NAME', 'OctoFox Library')[:80],
                'port': int(os.environ.get('OCTOFOX_PUBLISHED_PORT') or urlsplit(self.app.origin).port or 8080)}

    def answer(self, data, peer):
        if not local_address(peer) or len(data) > 1024:
            return None
        try:
            request = json.loads(data)
            if (not isinstance(request, dict) or request.get('product') != PRODUCT
                    or request.get('protocol') != 1 or not re.fullmatch(r'[a-f0-9]{32}', str(request.get('nonce', '')))):
                return None
            now = time.monotonic()
            with self.lock:
                start, count = self.rate.pop(peer, (now, 0))
                count = count + 1 if now - start < 60 else 1
                self.rate[peer] = (start if now - start < 60 else now, count)
                while len(self.rate) > 256:
                    self.rate.popitem(last=False)
                if count > 40:
                    return None
            info = self.metadata()
            response = info | {'nonce': request['nonce']}
            if request.get('action') == 'connect':
                target = request.get('origin', '')
                parsed = urlsplit(target)
                if (parsed.scheme != 'http' or not local_address(parsed.hostname or '')
                        or parsed.username or parsed.password or parsed.path or parsed.query or parsed.fragment
                        or parsed.port != info['port'] or target != f'http://{parsed.hostname}:{parsed.port}'):
                    return None
                ticket = secrets.token_urlsafe(32)
                with self.lock:
                    self.tickets = OrderedDict((k, v) for k, v in self.tickets.items() if v[2] > now)
                    self.tickets[ticket] = (peer, target, now + 3600)
                    while len(self.tickets) > 128:
                        self.tickets.popitem(last=False)
                response['ticket'] = ticket
                response['expiresIn'] = 3600
            elif request.get('action') != 'discover':
                return None
            return json.dumps(response).encode()
        except (ValueError, TypeError, UnicodeError):
            return None

    def request_origin(self, headers, peer):
        # A local reverse proxy must never turn a public request into a LAN claim.
        if not local_address(peer) or any(k.lower() in {'forwarded', 'x-forwarded-for', 'x-forwarded-host',
                'x-forwarded-proto', 'cf-connecting-ip', 'x-real-ip'} for k in headers):
            return None
        key = headers.get('X-OctoFox-Native', '')
        with self.lock:
            ticket = self.tickets.get(key)
        if not ticket or ticket[0] != peer or ticket[2] <= time.monotonic():
            return None
        if headers.get('Host') != urlsplit(ticket[1]).netloc:
            return None
        if headers.get('Origin') not in (None, ticket[1]):
            return None
        return ticket[1]

    def start(self):
        port = int(os.environ.get('OCTOFOX_DISCOVERY_PORT', str(PORT)))
        if not port:
            return
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind((os.environ.get('OCTOFOX_DISCOVERY_BIND', '0.0.0.0'), port))
        except OSError:
            sock.close()
            logging.warning('Desktop discovery could not bind its UDP port; web service remains available.')
            return
        self.socket = sock
        threading.Thread(target=self.serve, daemon=True, name='octofox-desktop-discovery').start()

    def serve(self):
        sock = self.socket
        while self.socket:
            try:
                data, peer = sock.recvfrom(1025)
                response = self.answer(data, peer[0])
                if response:
                    sock.sendto(response, peer)
            except ConnectionResetError:
                continue
            except OSError:
                return

    def close(self):
        sock, self.socket = self.socket, None
        if sock:
            sock.close()
