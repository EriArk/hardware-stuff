"""Private Docker adapter used only by the network-isolated maintenance worker."""
import http.client
import json
import os
import socket
import struct
import tarfile
import time
from urllib.parse import quote, urlencode

from .backup_archive import BackupError


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path='/var/run/docker.sock', timeout=120):
        super().__init__('localhost', timeout=timeout)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


class Docker:
    def __init__(self):
        self.version = None

    def prefix(self):
        if self.version is None:
            conn = UnixHTTP()
            try:
                conn.request('GET', '/version')
                response = conn.getresponse()
                if response.status != 200:
                    raise BackupError('The local Docker daemon is unavailable.')
                versions = json.loads(response.read())
                maximum = tuple(map(int, versions['ApiVersion'].split('.')))
                minimum = tuple(map(int, versions.get('MinAPIVersion', '1.24').split('.')))
                selected = min(maximum, (1, 45))
                if selected < max(minimum, (1, 41)):
                    raise BackupError('This Docker API version is not supported by the backup service.')
                self.version = '.'.join(map(str, selected))
            finally:
                conn.close()
        return '/v' + self.version

    def request(self, method, path, data=None, *, raw=False):
        conn = UnixHTTP(timeout=600)
        try:
            body = data if raw else json.dumps(data).encode() if data is not None else None
            headers = {'Content-Type': 'application/x-tar' if raw else 'application/json'}
            if raw and hasattr(body, 'fileno'):
                headers['Content-Length'] = str(os.fstat(body.fileno()).st_size)
            conn.request(method, self.prefix() + path, body, headers)
            response = conn.getresponse()
            payload = response.read()
            if response.status not in {200, 201, 204, 304}:
                raise BackupError('The server maintenance operation could not be completed.')
            return payload if raw else json.loads(payload) if payload else None
        finally:
            conn.close()

    def containers(self):
        own = self.request('GET', '/containers/' + quote(os.environ['HOSTNAME']) + '/json')
        labels = own['Config']['Labels']
        if labels.get('com.docker.compose.service') != 'backups':
            raise BackupError('Backup worker is not part of this installation.')
        project = labels['com.docker.compose.project']
        filters = json.dumps({'label': ['com.docker.compose.project=' + project,
            'com.docker.compose.config-hash', 'com.docker.compose.oneoff=False']})
        rows = self.request('GET', '/containers/json?' + urlencode({'all': 1, 'filters': filters}))
        found = {}
        for row in rows:
            if (not row['Labels'].get('com.docker.compose.config-hash')
                    or row['Labels'].get('com.docker.compose.oneoff') != 'False'):
                continue
            service = row['Labels'].get('com.docker.compose.service')
            if service in {'library', 'booklore', 'database'}:
                if service in found:
                    raise BackupError('Multiple server containers found. Finish the server update first.')
                found[service] = self.request('GET', '/containers/' + row['Id'] + '/json')
        if set(found) != {'library', 'booklore', 'database'}:
            raise BackupError('The complete server must be installed before making a backup.')
        # A project label alone must never authorize access to unrelated volumes.
        mounts = {m['Destination']: m.get('Name') for m in own['Mounts'] if m['Type'] == 'volume'}
        for service, destination, worker_path in [('library', '/data', '/data/library'),
                ('booklore', '/app/data', '/data/booklore'), ('booklore', '/books', '/data/catalog')]:
            actual = next((m.get('Name') for m in found[service]['Mounts'] if m['Destination'] == destination), None)
            if not actual or actual != mounts.get(worker_path):
                raise BackupError('Backup volume configuration does not match this server.')
        return found

    def stop(self, container):
        self.request('POST', '/containers/' + container['Id'] + '/stop?t=90')
        state = self.request('GET', '/containers/' + container['Id'] + '/json')['State']
        if state['Running'] or state.get('ExitCode') == 137:
            raise BackupError('A service did not shut down cleanly. Backup was cancelled.')

    def start(self, container):
        self.request('POST', '/containers/' + container['Id'] + '/start')

    def healthy(self, container, timeout=240):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.request('GET', '/containers/' + container['Id'] + '/json')['State']
            if state.get('Health', {}).get('Status') == 'healthy':
                return
            time.sleep(2)
        raise BackupError('The server has not become ready yet. Check the server before retrying.')

    def execute(self, container, command, output=None):
        execution = self.request('POST', '/containers/' + container['Id'] + '/exec', {
            'AttachStdout': True, 'AttachStderr': True, 'Cmd': command, 'User': '0', 'Tty': False})
        conn = UnixHTTP(timeout=600)
        try:
            conn.request('POST', self.prefix() + '/exec/' + execution['Id'] + '/start',
                         json.dumps({'Detach': False, 'Tty': False}), {'Content-Type': 'application/json'})
            response = conn.getresponse()
            if response.status != 200:
                raise BackupError('Account database operation failed.')
            while header := response.read(8):
                if len(header) != 8:
                    raise BackupError('Incomplete database response.')
                remaining = struct.unpack('>I', header[4:])[0]
                while remaining:
                    chunk = response.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise BackupError('Incomplete database response.')
                    if header[0] == 1 and output:
                        output.write(chunk)
                    remaining -= len(chunk)
        finally:
            conn.close()
        result = self.request('GET', '/exec/' + execution['Id'] + '/json')
        if result.get('Running') or result.get('ExitCode') != 0:
            raise BackupError('Account database operation failed.')

    def dump(self, container, path):
        with path.open('wb') as output:
            self.execute(container, ['sh', '-c',
                'exec mariadb-dump --user="$MYSQL_USER" --password="$MYSQL_PASSWORD" '
                '--single-transaction --hex-blob --skip-comments --databases booklore'], output)

    def restore(self, container, path):
        # A fixed temporary name and fixed command; no caller-supplied shell fragments.
        # Stream the tar from disk rather than loading a potentially large SQL dump.
        import tempfile
        with tempfile.TemporaryFile(dir='/backups/work') as payload:
            with tarfile.open(fileobj=payload, mode='w') as archive:
                info = tarfile.TarInfo('octofox-restore.sql')
                info.size = path.stat().st_size
                info.mode = 0o600
                with path.open('rb') as source:
                    archive.addfile(info, source)
            payload.seek(0)
            self.request('PUT', '/containers/' + container['Id'] + '/archive?path=/tmp', payload, raw=True)
        try:
            self.execute(container, ['sh', '-c',
                'mariadb --user="$MYSQL_USER" --password="$MYSQL_PASSWORD" '
                '-e "DROP DATABASE booklore"'])
            self.execute(container, ['sh', '-c',
                'exec mariadb --binary-mode=1 --local-infile=0 --user="$MYSQL_USER" --password="$MYSQL_PASSWORD" '
                '< /tmp/octofox-restore.sql'])
        finally:
            self.execute(container, ['rm', '-f', '/tmp/octofox-restore.sql'])
