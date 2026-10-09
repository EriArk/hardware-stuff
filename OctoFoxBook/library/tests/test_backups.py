from contextlib import closing
import hashlib
import io
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from urllib.error import HTTPError
from urllib.request import Request

from octofox_library import backup_archive as archive
from octofox_library.backup_worker import Worker
from octofox_library.backup_docker import Docker
from test_companion import CompanionHTTPCase


class ArchiveFixture:
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.roots = {n: self.root / n for n in archive.ROOTS}
        for p in self.roots.values(): p.mkdir()
        with closing(sqlite3.connect(self.roots['library'] / 'library.sqlite3')) as db:
            db.execute('CREATE TABLE reading (book, position)')
            db.execute('INSERT INTO reading VALUES (?,?)', ('example', 42))
            db.commit()
        (self.roots['library'] / 'uploads').mkdir()
        (self.roots['library'] / 'uploads' / 'story.fb2').write_bytes(b'book content')
        (self.roots['library'] / 'instance-id').write_text('keep-target-identity')
        (self.roots['library'] / 'speech-cache').mkdir()
        (self.roots['library'] / 'speech-cache' / 'audio').write_bytes(b'regenerated')
        self.sql = self.root / 'booklore.sql'; self.sql.write_bytes(b'account database')
        self.path = self.root / 'backup.zip'
        self.manifest = archive.create(self.path, self.roots, self.sql, created='2026-10-09',
                                       images={'booklore': 'booklore:test', 'database': 'database:test'})

    def rewrite(self, change):
        with zipfile.ZipFile(self.path) as source:
            files = {n: source.read(n) for n in source.namelist()}
        change(files)
        with zipfile.ZipFile(self.path, 'w') as target:
            for name, data in files.items(): target.writestr(name, data)


class ArchiveTests(ArchiveFixture, unittest.TestCase):
    def test_roundtrip_preserves_bytes_and_excludes_identity_and_audio(self):
        destination = self.root / 'out'; destination.mkdir()
        manifest = archive.inspect(self.path, destination)
        self.assertEqual(self.manifest, manifest)
        self.assertEqual((destination / 'library/uploads/story.fb2').read_bytes(), b'book content')
        self.assertFalse((destination / 'library/instance-id').exists())
        self.assertFalse((destination / 'library/speech-cache').exists())
        with closing(sqlite3.connect(destination / 'library/library.sqlite3')) as db:
            self.assertEqual(db.execute('SELECT position FROM reading').fetchone()[0], 42)

    def test_corrupted_file_is_rejected(self):
        self.rewrite(lambda files: files.update({'booklore.sql': b'broken data'}))
        with self.assertRaises(archive.BackupError): archive.inspect(self.path)

    def test_wrong_digest_is_rejected(self):
        def change(files):
            manifest = json.loads(files['manifest.json'])
            manifest['files']['booklore.sql']['sha256'] = '0' * 64
            files['manifest.json'] = json.dumps(manifest).encode()
        self.rewrite(change)
        with self.assertRaisesRegex(archive.BackupError, 'checksum'): archive.inspect(self.path)

    def test_traversal_and_special_names_rejected_even_with_matching_manifest(self):
        for name in ['library/../../outside', '/tmp/outside', 'library/a\\b', 'library/a/../b',
                     'library/instance-id', 'library//a', 'library/a:stream', 'unknown/a']:
            with self.subTest(name=name): self.assertFalse(archive.safe_name(name))
        def change(files):
            manifest = json.loads(files['manifest.json'])
            name = 'library/../../outside'
            files[name] = b'bad'
            manifest['files'][name] = {'bytes': 3, 'sha256': hashlib.sha256(b'bad').hexdigest()}
            files['manifest.json'] = json.dumps(manifest).encode()
        self.rewrite(change)
        with self.assertRaises(archive.BackupError): archive.inspect(self.path)
        self.assertFalse((self.root.parent / 'outside').exists())

    def test_unsupported_version_and_missing_database_rejected(self):
        def change(files):
            manifest = json.loads(files['manifest.json']); manifest['version'] = 999
            files['manifest.json'] = json.dumps(manifest).encode()
        self.rewrite(change)
        with self.assertRaises(archive.BackupError): archive.inspect(self.path)

    def test_symlink_zip_entry_rejected(self):
        with zipfile.ZipFile(self.path) as source:
            files = {n: source.read(n) for n in source.namelist()}
        with zipfile.ZipFile(self.path, 'w') as target:
            for name, data in files.items():
                info = zipfile.ZipInfo(name)
                if name == 'booklore.sql': info.external_attr = 0o120777 << 16
                target.writestr(info, data)
        with self.assertRaises(archive.BackupError): archive.inspect(self.path)

    def test_expansion_limit_is_enforced(self):
        with patch.object(archive, 'MAX_BYTES', 100):
            with self.assertRaises(archive.BackupError): archive.inspect(self.path)


class FakeDocker:
    def __init__(self): self.calls = []; self.sql = b'original accounts'; self.fail_restore = False
    def containers(self):
        return {n: {'Id': n, 'Config': {'Image': n + ':test'}, 'State': {'Running': True}}
                for n in ('library', 'booklore', 'database')}
    def stop(self, c): self.calls.append(('stop', c['Id']))
    def start(self, c): self.calls.append(('start', c['Id']))
    def healthy(self, c): self.calls.append(('healthy', c['Id']))
    def dump(self, c, p): p.write_bytes(self.sql)
    def restore(self, c, p):
        self.calls.append(('restore', c['Id']))
        self.sql = p.read_bytes()
        if self.fail_restore:
            self.fail_restore = False
            raise archive.BackupError('Injected database failure.')


class WorkerTests(ArchiveFixture, unittest.TestCase):
    # Reuse a real miniature library; test the state machine with a fake Docker boundary.
    def setUp(self):
        super().setUp()
        self.docker = FakeDocker()
        self.worker = Worker(self.root / 'store', self.roots, self.docker)
        self.key = 'a' * 32
        self.worker.path('inbox', self.key).write_bytes(self.path.read_bytes())
        self.worker.verify(self.key)

    def run_job(self, action, **extra):
        self.worker.job = {'id': 'b' * 32, 'token': 'c' * 64, 'action': action, 'state': 'running',
                           'source': self.key, 'created': '2026-10-09', **extra}
        with patch('octofox_library.backup_worker.time.sleep'):
            self.worker.run()

    def test_backup_pauses_writers_then_restarts_services(self):
        self.run_job('create')
        self.assertEqual(self.worker.job['state'], 'done')
        self.assertEqual(self.docker.calls[:2], [('stop', 'library'), ('stop', 'booklore')])
        self.assertEqual(self.docker.calls[-1], ('healthy', 'library'))
        self.assertEqual(len(self.worker.info()['archives']), 1)

    def test_restore_keeps_target_identity_and_restores_content(self):
        (self.roots['library'] / 'uploads' / 'story.fb2').write_bytes(b'changed')
        self.run_job('restore')
        self.assertEqual(self.worker.job['state'], 'done', self.worker.job)
        self.assertEqual((self.roots['library'] / 'uploads' / 'story.fb2').read_bytes(), b'book content')
        self.assertEqual((self.roots['library'] / 'instance-id').read_text(), 'keep-target-identity')
        self.assertTrue(self.worker.info()['archives'][0]['safety'])
        self.assertEqual(self.docker.sql, b'account database')

    def test_failed_restore_rolls_back_accounts_and_books(self):
        (self.roots['library'] / 'uploads' / 'story.fb2').write_bytes(b'latest book')
        self.docker.fail_restore = True
        self.run_job('restore')
        self.assertEqual(self.worker.job['state'], 'failed')
        self.assertIn('safety copy was restored', self.worker.job['error'])
        self.assertEqual(self.docker.sql, b'original accounts')
        self.assertEqual((self.roots['library'] / 'uploads' / 'story.fb2').read_bytes(), b'latest book')

    def test_damaged_restore_does_not_stop_or_change_server(self):
        self.worker.path('inbox', self.key).write_bytes(b'not a zip')
        self.run_job('restore')
        self.assertEqual(self.worker.job['state'], 'failed')
        self.assertEqual(self.docker.calls, [])

    def test_worker_restart_recovers_from_restore_journal(self):
        safety = self.worker.snapshot(self.docker.containers(), safety=True)
        (self.roots['library'] / 'uploads' / 'story.fb2').write_bytes(b'partial overwrite')
        self.worker.job = {'action': 'restore', 'state': 'running', 'safety': safety, 'applying': True}
        self.worker.run(recovering=True)
        self.assertEqual(self.worker.job['state'], 'failed')
        self.assertEqual((self.roots['library'] / 'uploads' / 'story.fb2').read_bytes(), b'book content')

    def test_progress_token_and_exclusive_operation(self):
        self.worker.job = {'action': 'create', 'state': 'running', 'token': 'c' * 64}
        with self.assertRaises(archive.BackupError): self.worker.command({'action': 'progress', 'token': 'wrong'})
        with self.assertRaises(archive.BackupError): self.worker.start('create')
        with self.assertRaises(archive.BackupError): self.worker.command({'action': 'delete', 'id': self.key})

    def test_incompatible_engine_is_rejected_before_pause(self):
        containers = self.docker.containers()
        containers['database']['Config']['Image'] = 'database:other-version'
        with patch.object(self.docker, 'containers', return_value=containers):
            self.run_job('restore')
        self.assertEqual(self.worker.job['state'], 'failed')
        self.assertEqual(self.docker.calls, [])

    def test_recovery_required_cannot_be_overwritten_by_another_job(self):
        self.worker.job = {'state': 'recovery-required'}
        with self.assertRaises(archive.BackupError): self.worker.start('create')


class BackupHTTPTests(CompanionHTTPCase):
    def test_backup_actions_require_admin_and_csrf(self):
        self.assertEqual(self.call('/companion-api/backups')[0], 401)
        self.assertEqual(self.call('/companion-api/backups/create', {})[0], 401)
        self.setup_admin()
        self.assertEqual(self.call('/companion-api/backups/create', {}, {'X-CSRF-Token': 'bad'})[0], 403)
        with patch.object(self.app.companion.backups, 'call', return_value={'id': 'a' * 32}) as call:
            self.assertEqual(self.call('/companion-api/backups/create', {})[0], 202)
            call.assert_called_once_with('create')

    def test_restore_requires_explicit_confirmation(self):
        self.setup_admin()
        with patch.object(self.app.companion.backups, 'call') as call:
            self.assertEqual(self.call('/companion-api/backups/restore', {'id': 'a'*32})[0], 400)
            call.assert_not_called()

    def test_progress_survives_session_restart_but_requires_operation_secret(self):
        self.assertEqual(self.call('/companion-api/backups/progress', {'token': 'wrong'})[0], 403)
        with patch.object(self.app.companion.backups, 'call', return_value={'state': 'done'}):
            self.assertEqual(self.call('/companion-api/backups/progress', {'token': 'c'*64})[0], 200)
            self.assertEqual(self.call('/companion-api/backups/progress', {'token': 'c'*64}, {'Origin': 'https://evil.test'})[0], 403)

    def test_unavailable_worker_is_explained_and_bad_size_rejected(self):
        self.setup_admin()
        self.assertFalse(self.call('/companion-api/backups')[1]['available'])
        self.assertEqual(self.call('/companion-api/backups/upload', {'bytes': 21*1024**3})[0], 400)

    def test_streamed_upload_checks_offsets_length_and_cancel(self):
        self.setup_admin()
        backups = self.app.companion.backups
        backups.store = Path(self.temp.name) / 'backup-store'
        (backups.store / 'inbox').mkdir(parents=True)
        with patch.object(backups, 'call', return_value={'job': None}):
            status, data, _ = self.call('/companion-api/backups/upload', {'bytes': 7})
            self.assertEqual(status, 201)
            key = data['id']
            def chunk(offset, payload):
                request = Request(self.base + '/companion-api/backups/upload/' + key + '/chunk', payload,
                    {'Origin': self.base, 'Content-Type': 'application/octet-stream',
                     'X-CSRF-Token': self.csrf, 'X-Upload-Offset': str(offset)})
                try: response = self.client.open(request)
                except HTTPError as error: response = error
                with response: return response.status, json.load(response)
            self.assertEqual(chunk(0, b'abc'), (200, {'offset': 3}))
            self.assertEqual(chunk(0, b'abc')[0], 409)
            self.assertEqual(self.call('/companion-api/backups/upload/' + key + '/finish', {})[0], 409)
            self.assertEqual(chunk(3, b'defg'), (200, {'offset': 7}))
            self.assertEqual((backups.store / 'inbox' / (key + '.zip')).read_bytes(), b'abcdefg')
            self.assertEqual(self.call('/companion-api/backups/upload/' + key + '/cancel', {})[0], 200)
            self.assertFalse((backups.store / 'inbox' / (key + '.zip')).exists())

    def test_demoted_owner_cannot_download_backup(self):
        self.setup_admin()
        self.state['users']['admin']['permissions']['admin'] = False
        self.assertEqual(self.call('/companion-api/backups/' + 'a'*32 + '/download')[0], 403)


class DockerScopeTests(unittest.TestCase):
    def test_ad_hoc_test_containers_with_inherited_image_labels_are_not_server_services(self):
        adapter = Docker()
        own = {'Config': {'Labels': {'com.docker.compose.service': 'backups', 'com.docker.compose.project': 'test'}},
               'Mounts': [{'Destination': '/data/' + name, 'Name': name, 'Type': 'volume'} for name in archive.ROOTS]}
        rows = [{'Id': name, 'Labels': {'com.docker.compose.service': name, 'com.docker.compose.config-hash': 'hash',
                 'com.docker.compose.oneoff': 'False'}} for name in ['library', 'booklore', 'database']]
        rows.append({'Id': 'unit-test', 'Labels': {'com.docker.compose.service': 'library'}})
        containers = {name: {'Config': {}, 'Mounts': []} for name in ['library', 'booklore', 'database']}
        containers['library']['Mounts'] = [{'Destination': '/data', 'Name': 'library'}]
        containers['booklore']['Mounts'] = [{'Destination': '/app/data', 'Name': 'booklore'}, {'Destination': '/books', 'Name': 'catalog'}]
        def request(method, path):
            if path == '/containers/worker/json': return own
            if path.startswith('/containers/json?'):
                self.assertIn('com.docker.compose.config-hash', path)
                return rows
            return containers[path.split('/')[2]]
        with patch.dict('os.environ', {'HOSTNAME': 'worker'}), patch.object(adapter, 'request', side_effect=request):
            self.assertEqual(set(adapter.containers()), {'library', 'booklore', 'database'})


if __name__ == '__main__': unittest.main()
