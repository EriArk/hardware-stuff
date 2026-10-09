"""Offline snapshots controlled by an authenticated Companion over a private socket.

Only this network-isolated service can access Docker. The web service cannot send
Docker commands, container IDs, paths or SQL to it; actions operate on fixed mounts.
"""
from contextlib import closing
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socketserver
import sqlite3
import threading
import time

from . import backup_archive as archive
from .backup_archive import BackupError
from .backup_docker import Docker

ID = re.compile(r'[a-f0-9]{32}')
STORE = Path('/backups')
ROOTS = {name: Path('/data') / name for name in archive.ROOTS}


def identifier(value):
    if not isinstance(value, str) or not ID.fullmatch(value):
        raise BackupError('Invalid backup identifier.')
    return value


def now():
    return datetime.now(timezone.utc).isoformat(timespec='seconds')


def write_json(path, value):
    temp = path.with_suffix('.tmp')
    with temp.open('w', encoding='utf-8') as output:
        temp.chmod(0o600)
        output.write(json.dumps(value))
        output.flush()
        os.fsync(output.fileno())
    os.replace(temp, path)
    archive.sync_directory(path.parent)


class Worker:
    def __init__(self, store=STORE, roots=None, docker=None):
        self.store, self.roots, self.docker = store, roots or ROOTS, docker or Docker()
        self.lock = threading.RLock()
        self.job = None
        for name in ('archives', 'inbox', 'work'):
            (store / name).mkdir(parents=True, exist_ok=True)
            if hasattr(os, 'chown'):
                os.chown(store / name, 1000, 1000)
            (store / name).chmod(0o700)
        saved = store / 'job.json'
        if saved.exists():
            self.job = json.loads(saved.read_text())

    def save(self, **changes):
        with self.lock:
            self.job.update(changes)
            write_json(self.store / 'job.json', self.job)

    def path(self, kind, key):
        return self.store / kind / (identifier(key) + '.zip')

    def public_job(self):
        with self.lock:
            return {k: v for k, v in self.job.items() if k not in {'source', 'applying'}} if self.job else None

    def summary(self, key):
        path = self.path('archives', key)
        sidecar = path.with_suffix('.json')
        meta = json.loads(sidecar.read_text())
        return {'id': key, 'bytes': path.stat().st_size, **meta}

    def info(self):
        rows = []
        active = self.job.get('source') if self.job and self.job['state'] == 'running' else None
        for path in (self.store / 'inbox').glob('*'):
            if path.stem != active and time.time() - path.stat().st_mtime > 86400:
                path.unlink(missing_ok=True)
        for path in (self.store / 'archives').glob('*.zip'):
            try:
                rows.append(self.summary(path.stem))
            except (OSError, ValueError):
                continue
        return {'available': True, 'archives': sorted(rows, key=lambda r: r['created'], reverse=True),
                'job': self.public_job(), 'freeBytes': shutil.disk_usage(self.store).free}

    def verify(self, key):
        path = self.path('inbox', key)
        manifest = archive.inspect(path)
        containers = self.docker.containers()
        for name in ('booklore', 'database'):
            if manifest.get('images', {}).get(name) != containers[name]['Config']['Image']:
                raise BackupError('This backup needs the same BookLore and database versions as its source server.')
        result = {'id': key, 'created': manifest['created'], 'files': len(manifest['files']),
                  'bytes': sum(f['bytes'] for f in manifest['files'].values()), 'sha256': archive.digest(path)}
        write_json(path.with_suffix('.json'), result)
        return result

    def start(self, action, key=None):
        if action not in {'create', 'restore', 'verify'}:
            raise BackupError('Unknown backup action.')
        with self.lock:
            if self.job and self.job['state'] in {'running', 'recovery-required'}:
                raise BackupError('A backup operation is already running.')
            if len(list((self.store / 'archives').glob('*.zip'))) >= 64 and action != 'verify':
                raise BackupError('Keep up to 64 backups on this server. Download and remove an older copy first.')
            if action in {'restore', 'verify'}:
                key = identifier(key)
            if action == 'restore':
                if not self.path('inbox', key).with_suffix('.json').is_file():
                    raise BackupError('Upload and check this backup before restoring it.')
            self.job = {'id': secrets.token_hex(16), 'token': secrets.token_hex(32),
                        'action': action, 'source': key, 'state': 'running', 'phase': 'Preparing', 'created': now()}
            write_json(self.store / 'job.json', self.job)
            threading.Thread(target=self.run, daemon=True).start()
            return self.public_job()

    def snapshot(self, containers, *, safety=False):
        key = secrets.token_hex(16)
        sql = self.store / 'work' / 'snapshot.sql'
        self.docker.dump(containers['database'], sql)
        path = self.path('archives', key)
        try:
            manifest = archive.create(path, self.roots, sql, created=now(),
                images={n: c['Config']['Image'] for n, c in containers.items()})
            archive.inspect(path)
            write_json(path.with_suffix('.json'), {'created': manifest['created'], 'safety': safety,
                'files': len(manifest['files']), 'sha256': archive.digest(path)})
            if hasattr(os, 'chown'):
                os.chown(path, 1000, 1000)
            path.chmod(0o600)
            return key
        finally:
            sql.unlink(missing_ok=True)

    def apply(self, path, containers):
        stage = self.store / 'work' / 'restore'
        if stage.exists():
            shutil.rmtree(stage)
        stage.mkdir()
        archive.inspect(path, stage)
        # Reject broken SQLite before touching any destination. The expression
        # indexes use the library's normalization function.
        from .web_catalog import normalized
        with closing(sqlite3.connect(stage / 'library/library.sqlite3')) as db:
            db.create_function('normalized', 1, normalized, deterministic=True)
            if db.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise BackupError('The library database failed its integrity check.')
        self.docker.restore(containers['database'], stage / 'booklore.sql')
        for label, root in self.roots.items():
            for item in root.iterdir():
                if label == 'library' and item.name in {'instance-id', 'companion-setup-key'}:
                    continue
                if item.is_dir() and not item.is_symlink():
                    shutil.rmtree(item)
                else:
                    item.unlink()
            source = stage / label
            if source.exists():
                for item in source.iterdir():
                    shutil.move(str(item), root / item.name)
            if hasattr(os, 'chown'):
                for item in root.rglob('*'):
                    os.chown(item, 1000, 1000)
        shutil.rmtree(stage)

    def resume(self, containers):
        self.docker.start(containers['booklore'])
        self.docker.healthy(containers['booklore'])
        self.docker.start(containers['library'])
        self.docker.healthy(containers['library'])

    def run(self, recovering=False):
        containers = None
        paused = recovering
        try:
            if recovering:
                containers = self.docker.containers()
                if self.job.get('applying'):
                    self.docker.stop(containers['library'])
                    self.docker.stop(containers['booklore'])
                    self.apply(self.path('archives', self.job['safety']), containers)
                self.resume(containers)
                self.save(state='failed', phase='Interrupted operation recovered', applying=False,
                          error='The operation was interrupted. The previous server data was retained or restored.')
                return
            time.sleep(2)  # Let the initiating HTTP response reach the browser.
            if self.job['action'] == 'verify':
                result = self.verify(self.job['source'])
                self.save(state='done', phase='Backup checked', preview=result)
                return
            containers = self.docker.containers()
            if any(not c['State']['Running'] for c in containers.values()):
                raise BackupError('Start the complete server before creating or restoring a backup.')
            needed = sum(p.stat().st_size for root in self.roots.values() for p in root.rglob('*') if p.is_file())
            if self.job['action'] == 'restore':
                preview_path = self.path('inbox', self.job['source']).with_suffix('.json')
                expected = json.loads(preview_path.read_text())['sha256']
                preview = self.verify(self.job['source'])
                if preview['sha256'] != expected:
                    preview_path.unlink(missing_ok=True)
                    raise BackupError('The uploaded backup changed. Upload and check it again.')
                needed += preview['bytes']
            if shutil.disk_usage(self.store).free < needed * 2 + 256 * 1024**2:
                raise BackupError('Not enough free space for a verified backup and a safe restore.')
            paused = True
            self.save(phase='Pausing the library')
            self.docker.stop(containers['library'])
            self.docker.stop(containers['booklore'])
            self.save(phase='Saving accounts, books and reading progress')
            key = self.snapshot(containers, safety=self.job['action'] == 'restore')
            self.save(archive=key)
            if self.job['action'] == 'restore':
                self.save(safety=key, applying=True, phase='Restoring the checked backup')
                self.apply(self.path('inbox', self.job['source']), containers)
            self.save(phase='Starting the library')
            self.resume(containers)
            self.save(state='done', phase='Restore complete' if self.job['action'] == 'restore' else 'Backup ready', applying=False)
        except Exception as error:
            if self.job['action'] == 'verify':
                self.path('inbox', self.job['source']).unlink(missing_ok=True)
            message = str(error) if isinstance(error, BackupError) else 'The operation failed. Check the server and available disk space.'
            try:
                if containers and paused:
                    if self.job.get('applying'):
                        self.save(phase='Restoring the safety copy')
                        self.docker.stop(containers['library'])
                        self.docker.stop(containers['booklore'])
                        self.apply(self.path('archives', self.job['safety']), containers)
                        message += ' The safety copy was restored.'
                    self.resume(containers)
                self.save(state='failed', phase='Operation stopped', error=message, applying=False)
            except Exception:
                # Keep the journal and stopped services for recovery at worker restart.
                self.save(state='recovery-required', phase='Recovery needed', error=message +
                    ' Automatic recovery could not finish. The safety archive is retained; restart the backup service to retry recovery.')

    def command(self, data):
        action = data.get('action')
        if action == 'info':
            return self.info()
        if action == 'progress':
            if not self.job or not secrets.compare_digest(str(data.get('token', '')), self.job['token']):
                raise BackupError('This operation is no longer available. Sign in to view backups.')
            return self.public_job()
        if action in {'create', 'restore', 'verify'}:
            return self.start(action, data.get('id'))
        if action == 'delete':
            with self.lock:
                if self.job and self.job['state'] in {'running', 'recovery-required'}:
                    raise BackupError('Wait for the current operation to finish before removing backups.')
                path = self.path('archives', data.get('id'))
                path.unlink()
                path.with_suffix('.json').unlink(missing_ok=True)
            return {'ok': True}
        raise BackupError('Unknown backup action.')


def main():
    worker = Worker()
    control = Path('/run/backups/control.sock')
    control.parent.mkdir(parents=True, exist_ok=True)
    control.unlink(missing_ok=True)

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            self.connection.settimeout(30)
            try:
                line = self.rfile.readline(8193)
                if not line:
                    return
                if len(line) > 8192:
                    raise BackupError('Invalid backup request.')
                result = worker.command(json.loads(line))
                value = {'ok': True, 'result': result}
            except (BackupError, OSError, ValueError, KeyError) as error:
                value = {'ok': False, 'error': str(error) if isinstance(error, BackupError) else 'The backup operation is unavailable.'}
            self.wfile.write(json.dumps(value).encode() + b'\n')

    class Server(socketserver.ThreadingUnixStreamServer):
        daemon_threads = True

    with Server(str(control), Handler) as server:
        os.chown(control, 1000, 1000)
        control.chmod(0o600)
        if worker.job and worker.job['state'] in {'running', 'recovery-required'}:
            threading.Thread(target=worker.run, kwargs={'recovering': True}, daemon=True).start()
        server.serve_forever()


if __name__ == '__main__':
    main()
