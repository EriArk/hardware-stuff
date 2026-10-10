"""Separate revocable OPDS credentials for each physical reader."""
import base64
import hashlib
import re
import secrets
import time
from urllib.parse import urlsplit
from octofox_library.web_errors import WebError


class ReaderPairing:
    def __init__(self, companion):
        self.companion = companion
        self.app = companion.app
        with self.app.db() as db:
            db.execute('''CREATE TABLE IF NOT EXISTS reader_keys (
                username TEXT PRIMARY KEY, digest TEXT NOT NULL, owner TEXT NOT NULL,
                device TEXT NOT NULL, upstream_id INTEGER NOT NULL, created REAL NOT NULL,
                revoked INTEGER NOT NULL DEFAULT 0)''')

    def resolve(self, username, password, *, allowed=False, device=None):
        with self.app.db() as db:
            row = db.execute('SELECT * FROM reader_keys WHERE username=?', (username,)).fetchone()
        if row is None: return username
        if (not allowed or row['revoked'] or
                not secrets.compare_digest(row['digest'], hashlib.sha256(password.encode()).hexdigest()) or
                (device is not None and device != row['device'])):
            raise WebError(401, 'Device pairing is invalid or has been revoked')
        return row['owner']

    def list(self):
        with self.app.db() as db:
            rows = db.execute('SELECT username,owner,device,created,revoked FROM reader_keys ORDER BY created DESC').fetchall()
        return {'readers': [dict(row) for row in rows]}

    def revoke(self, username):
        if not isinstance(username, str): raise WebError(400, 'Invalid reader')
        with self.app.db() as db:
            result = db.execute('UPDATE reader_keys SET revoked=1 WHERE username=?', (username,))
            if not result.rowcount: raise WebError(404, 'Reader not found')
        return {'ok': True}

    def create(self, values, admin_token):
        device = values.get('device', '')
        if not isinstance(device, str) or not re.fullmatch(r'reader-[a-f0-9]{12}', device):
            raise WebError(400, 'Connect an AbyssBook reader by USB first')
        username, password = values.get('username'), values.get('password')
        if not isinstance(username, str) or not username or not isinstance(password, str):
            raise WebError(400, 'Choose a library account')
        origin = values.get('origin', '')
        if origin not in self.app.network.allowed() or urlsplit(origin).scheme != 'https':
            raise WebError(400, 'Choose the configured public HTTPS address for this library')
        self.companion.accounts.guard(username)
        self.companion.enable_reader_service(admin_token)
        administrator = self.companion.api.request('/api/v1/users/me', token=admin_token)
        if administrator.get('username') == username:
            token = admin_token  # The current authenticated owner needs no second login.
        else:
            if not password: raise WebError(400, 'Enter the selected account password')
            token = self.companion.api.login(username, password)['accessToken']
        user = self.companion.api.request('/api/v1/users/me', token=token)
        if user.get('username') != username: raise WebError(409, 'Account identity changed')
        key_user = 'octodev_' + secrets.token_hex(12)
        secret = secrets.token_urlsafe(32)
        created = self.companion.api.request('/api/v2/opds-users',
            dict(username=key_user, password=secret, sortOrder='RECENT'), token=token)
        if not isinstance(created, dict) or type(created.get('id')) is not int:
            raise WebError(502, 'Could not create the device credential')
        authorization = 'Basic ' + base64.b64encode(f'{key_user}:{secret}'.encode()).decode()
        try:
            self.app.client.fetch_xml('', (), authorization)
            with self.app.db() as db:
                db.execute('INSERT INTO reader_keys VALUES (?,?,?,?,?,?,0)',
                    (key_user, hashlib.sha256(secret.encode()).hexdigest(), username, device, created['id'], time.time()))
                db.execute('INSERT OR IGNORE INTO devices VALUES (?,?,?,?)', (username, device, 'AbyssBook', time.time()))
        except Exception:
            # The account password and the new secret are never persisted here.
            try: self.companion.api.request(f"/api/v2/opds-users/{created['id']}", token=token, method='DELETE')
            except Exception: pass
            raise
        return dict(device=device, account=username, username=key_user, key=secret,
                    url=origin+'/reader-api/device', origin=origin)
