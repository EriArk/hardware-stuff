"""Administrator-only backup API. Archive bytes are streamed to private storage."""
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import threading
import time

from .web_errors import WebError

MAX_UPLOAD = 20 * 1024**3
CHUNK = 8 * 1024**2


class Backups:
    def __init__(self):
        self.socket = os.environ.get('OCTOFOX_BACKUP_SOCKET', '')
        self.store = Path(os.environ.get('OCTOFOX_BACKUP_STORE', '/backups'))
        self.lock = threading.Lock()
        self.uploads = {}

    def call(self, action, **values):
        if not self.socket:
            raise WebError(503, 'Backups are not enabled on this server. Start the updated server package first.')
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
                conn.settimeout(30)
                conn.connect(self.socket)
                conn.sendall(json.dumps({'action': action, **values}).encode() + b'\n')
                with conn.makefile('rb') as source:
                    line = source.readline(4 * 1024**2)
            data = json.loads(line)
            if not data['ok']:
                raise WebError(409, data['error'])
            return data['result']
        except (OSError, ValueError, KeyError):
            raise WebError(503, 'The backup service is unavailable. Check that the complete server is running.') from None

    def progress(self, handler):
        data = handler.body()
        token = data.get('token')
        if not isinstance(token, str) or not re.fullmatch('[a-f0-9]{64}', token):
            raise WebError(403, 'Invalid backup operation token.')
        return handler.send(200, self.call('progress', token=token))

    def route(self, handler, path, user):
        get = handler.command == 'GET'
        if path == '/companion-api/backups' and get:
            if not self.socket:
                return handler.send(200, {'available': False, 'archives': [], 'job': None})
            return handler.send(200, self.call('info'))
        if path == '/companion-api/backups/create' and not get:
            handler.body()
            return handler.send(202, self.call('create'))
        if path == '/companion-api/backups/restore' and not get:
            data = handler.body()
            if data.get('confirm') != 'RESTORE':
                raise WebError(400, 'Confirm that this backup will replace the current server data.')
            return handler.send(202, self.call('restore', id=data.get('id')))
        if path == '/companion-api/backups/upload' and not get:
            data = handler.body()
            size = data.get('bytes')
            if type(size) is not int or not 1 <= size <= MAX_UPLOAD:
                raise WebError(400, 'Choose an OctoFox backup ZIP up to 20 GB.')
            info = self.call('info')
            if (info.get('job') or {}).get('state') in {'running', 'recovery-required'}:
                raise WebError(409, 'Wait for the current backup operation to finish.')
            if shutil.disk_usage(self.store).free < size + 256 * 1024**2:
                raise WebError(409, 'There is not enough disk space to upload this backup.')
            with self.lock:
                self.expire_uploads()
                if self.uploads:
                    raise WebError(409, 'Another backup upload is in progress. Cancel it or try again later.')
                key = secrets.token_hex(16)
                path = self.store / 'inbox' / (key + '.zip')
                with path.open('xb'):
                    pass
                path.chmod(0o600)
                self.uploads[key] = {'user': user['id'], 'size': size, 'offset': 0, 'updated': time.time()}
            return handler.send(201, {'id': key, 'chunkBytes': CHUNK})
        match = re.fullmatch(r'/companion-api/backups/upload/([a-f0-9]{32})/(chunk|finish|cancel)', path)
        if match and not get:
            key, action = match.groups()
            with self.lock:
                upload = self.uploads.get(key)
                if not upload or upload['user'] != user['id']:
                    raise WebError(404, 'This upload expired. Choose the backup file again.')
                target = self.store / 'inbox' / (key + '.zip')
                if action == 'cancel':
                    handler.body()
                    target.unlink(missing_ok=True)
                    del self.uploads[key]
                    return handler.send(200, {'ok': True})
                if action == 'finish':
                    handler.body()
                    if upload['offset'] != upload['size']:
                        raise WebError(409, 'The upload is incomplete.')
                    result = self.call('verify', id=key)
                    del self.uploads[key]
                    return handler.send(202, result)
                length = int(handler.headers.get('Content-Length', '0'))
                offset = int(handler.headers.get('X-Upload-Offset', '-1'))
                if (handler.headers.get('Transfer-Encoding') or not 0 < length <= CHUNK
                        or offset != upload['offset'] or offset + length > upload['size']):
                    raise WebError(409, 'The upload chunk is out of order or too large. Choose the file again.')
                try:
                    with target.open('r+b') as output:
                        output.seek(offset)
                        remaining = length
                        while remaining:
                            chunk = handler.rfile.read(min(remaining, 1024 * 1024))
                            if not chunk:
                                raise WebError(400, 'The upload was interrupted.')
                            output.write(chunk)
                            remaining -= len(chunk)
                        output.truncate()
                except Exception:
                    with target.open('r+b') as output:
                        output.truncate(offset)
                    raise
                upload.update(offset=offset + length, updated=time.time())
                return handler.send(200, {'offset': upload['offset']})
        match = re.fullmatch(r'/companion-api/backups/([a-f0-9]{32})/(download|delete)', path)
        if match:
            key, action = match.groups()
            if action == 'delete' and not get:
                return handler.send(200, self.call('delete', id=key))
            if action == 'download' and get:
                path = self.store / 'archives' / (key + '.zip')
                try:
                    source = path.open('rb')
                except FileNotFoundError:
                    raise WebError(404, 'Backup not found.') from None
                with source:
                    handler.send_response(200)
                    handler.send_header('Content-Type', 'application/zip')
                    handler.send_header('Content-Length', str(os.fstat(source.fileno()).st_size))
                    handler.send_header('Content-Disposition', 'attachment; filename="OctoFox-backup-' + key + '.zip"')
                    handler.send_header('Cache-Control', 'no-store')
                    handler.send_header('X-Content-Type-Options', 'nosniff')
                    handler.end_headers()
                    shutil.copyfileobj(source, handler.wfile, length=1024 * 1024)
                return
        raise WebError(404, 'Backup action not found.')

    def expire_uploads(self):
        for key, item in list(self.uploads.items()):
            if time.time() - item['updated'] > 3600:
                (self.store / 'inbox' / (key + '.zip')).unlink(missing_ok=True)
                del self.uploads[key]
