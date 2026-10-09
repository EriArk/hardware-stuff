"""Installer boundaries: no real package installation, Docker changes, or account access."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch, Mock
import urllib.request
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import installer as m
import dependencies as d


class FakeInstallation:
    calls = []

    def __init__(self, root, project, progress):
        self.root, self.project = Path(root), project

    def prerequisites(self):
        pass

    def run(self, *args, **kw):
        return 'x86_64'

    def guard_project(self):
        pass

    def ports(self, settings, rows):
        pass

    def settings(self):
        return {'origin': 'http://localhost:18080', 'bind': '0.0.0.0', 'port': 18080,
                'admin_port': 18081, 'discovery_port': 49645}

    def rows(self):
        return []

    def start(self, args):
        self.calls.append(args)
        path = self.root / '.env'
        if not path.exists():
            path.write_text('DO_NOT_REPLACE=secret\n')


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        for name in m.ROOT_FILES:
            (self.source / name).write_text(name)
        for name in ('src', 'speech', 'tools'):
            (self.source / name).mkdir()
            (self.source / name / 'sample.py').write_text('print("hello")')
        self.target = self.root / 'server'
        self.runtime = Mock()
        self.runtime.ready.return_value = True
        self.app = m.Installer(self.source, FakeInstallation, self.runtime)
        self.options = {'directory': str(self.target), 'speech': True, 'port': 18080, 'adminPort': 18081}
        FakeInstallation.calls = []

    def test_first_install_and_restart_keep_environment_and_speech(self):
        self.app.submit('install', self.options).join(5)
        self.assertIsNone(self.app.snapshot()['error'])
        self.assertTrue(FakeInstallation.calls[0].speech)
        before = (self.target / '.env').read_bytes()
        self.app.submit('install', self.options).join(5)
        self.assertIsNone(self.app.snapshot()['error'])
        self.assertEqual((self.target / '.env').read_bytes(), before)
        self.assertIsNone(FakeInstallation.calls[-1].port)
        self.assertNotIn('secret', json.dumps(self.app.snapshot()))
        self.assertTrue(self.app.snapshot()['result']['existing'])

    def test_check_is_read_only_and_missing_runtime_does_not_block_install(self):
        self.runtime.ready.return_value = False
        self.app.submit('check', self.options).join(5)
        self.runtime.ensure.assert_not_called()
        self.assertFalse(self.target.exists())
        self.assertTrue(self.app.snapshot()['result']['needsRuntime'])
        self.app.submit('install', self.options).join(5)
        self.runtime.ensure.assert_called_once()
        self.assertIsNone(self.app.snapshot()['error'])

    def test_unknown_or_modified_folder_is_never_overwritten(self):
        self.target.mkdir()
        (self.target / 'my-books.txt').write_text('keep')
        self.app.submit('install', self.options).join(5)
        self.assertIsNotNone(self.app.snapshot()['error'])
        self.runtime.ensure.assert_not_called()
        self.assertEqual((self.target / 'my-books.txt').read_text(), 'keep')
        (self.target / 'my-books.txt').unlink()
        m.prepare(self.target, self.source)
        (self.target / 'compose.yaml').write_text('custom')
        self.app.submit('install', self.options).join(5)
        self.assertIn('changed', self.app.snapshot()['error'])
        self.assertEqual((self.target / 'compose.yaml').read_text(), 'custom')

    def test_payload_excludes_secrets_caches_and_data(self):
        (self.source / '.env').write_text('secret')
        (self.source / 'data').mkdir()
        (self.source / 'data/private.db').write_text('private')
        (self.source / 'src/__pycache__').mkdir()
        (self.source / 'src/__pycache__/sample.pyc').write_bytes(b'cache')
        m.prepare(self.target, self.source)
        self.assertFalse((self.target / '.env').exists())
        self.assertFalse((self.target / 'data').exists())
        self.assertFalse((self.target / 'src/__pycache__').exists())

    def test_cancelled_permissions_and_unexpected_errors_do_not_leak_details(self):
        self.runtime.ensure.side_effect = d.DependencyError('Permission was cancelled. Retry.')
        self.app.submit('install', self.options).join(5)
        self.assertIn('cancelled', self.app.snapshot()['error'])
        self.assertFalse(self.target.exists())
        self.runtime.ensure.side_effect = ValueError('password=private')
        self.app.submit('install', self.options).join(5)
        self.assertNotIn('private', self.app.snapshot()['error'])

    def test_concurrent_action_rejected_and_busy_resets_after_failure(self):
        gate = threading.Event()
        self.runtime.ensure.side_effect = lambda *a: gate.wait(3)
        thread = self.app.submit('install', self.options)
        with self.assertRaises(m.LaunchError):
            self.app.submit('install', self.options)
        gate.set(); thread.join(5)
        self.assertFalse(self.app.snapshot()['busy'])

    def test_relative_path_and_disk_root_rejected_before_system_setup(self):
        for name in ('relative/folder', str(self.root.anchor)):
            self.app.submit('install', dict(self.options, directory=name)).join(5)
            self.assertIsNotNone(self.app.snapshot()['error'])
        self.runtime.ensure.assert_not_called()

    def test_http_requires_local_host_origin_and_ephemeral_authorization(self):
        server = m.make_server(self.app)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        def request(path, *, token=True, origin=None, host=None, post=None):
            headers = {'Authorization': 'Bearer ' + server.token} if token else {}
            if origin is not None: headers['Origin'] = origin
            if host is not None: headers['Host'] = host
            if post is not None: headers['Content-Type'] = 'application/json'
            req = urllib.request.Request(server.origin + path, headers=headers, data=json.dumps(post).encode() if post is not None else None)
            try:
                with urllib.request.urlopen(req, timeout=3) as response:
                    return response.status, response.read()
            except urllib.error.HTTPError as error:
                return error.code, error.read()
        self.assertEqual(request('/api/state', token=False)[0], 403)
        self.assertEqual(request('/api/state', host='evil.example')[0], 403)
        self.assertEqual(request('/api/install', origin='https://evil.example', post=self.options)[0], 403)
        self.assertEqual(request('/api/install', post=self.options)[0], 403)
        self.assertFalse(self.target.exists())
        self.assertEqual(request('/api/check', origin=server.origin, post=self.options)[0], 202)
        self.assertEqual(request('/../installer.py')[0], 404)
        self.assertEqual(request('/api/state')[0], 200)


class DependencyTests(unittest.TestCase):
    def test_windows_reboot_stops_before_docker_download(self):
        with patch.object(d, 'refresh_path'), patch.object(d.Path, 'is_file', return_value=False), patch.object(d, 'run', return_value=Mock(returncode=1)), patch.object(d, 'powershell', return_value=Mock(returncode=3010)), patch.object(d, 'download') as download:
            with self.assertRaisesRegex(d.DependencyError, 'restart'):
                d.install_windows(Mock())
        download.assert_not_called()

    def test_ready_runtime_is_not_reinstalled(self):
        with patch.object(d, 'ready', return_value=True), patch.object(d, 'install_windows') as install:
            d.ensure(Mock())
        install.assert_not_called()

    def test_remote_context_is_not_modified(self):
        with patch.object(d, 'ready', return_value=False), patch.dict(d.os.environ, {'DOCKER_HOST':'ssh://remote'}, clear=True), patch.object(d, 'install_linux') as install:
            with self.assertRaisesRegex(d.DependencyError, 'local Docker'):
                d.ensure(Mock())
        install.assert_not_called()

    def test_failed_download_signature_never_executes_installer(self):
        completed = Mock(returncode=0)
        with patch.object(d, 'refresh_path'), patch.object(d.Path, 'is_file', return_value=False), patch.object(d, 'run', return_value=completed) as native, patch.object(d, 'download'), patch.object(d, 'powershell', side_effect=[completed, Mock(returncode=1)]):
            with self.assertRaisesRegex(d.DependencyError, 'signature'):
                d.install_windows(Mock())
        self.assertEqual([call.args[0] for call in native.call_args_list], [['wsl.exe', '--version']])

    def test_download_redirect_is_restricted(self):
        handler = d.OfficialRedirect()
        with self.assertRaises(d.DependencyError):
            handler.redirect_request(None, None, 302, '', {}, 'http://desktop.docker.com/package')
        with self.assertRaises(d.DependencyError):
            handler.redirect_request(None, None, 302, '', {}, 'https://other.example/package')

    def test_shell_values_are_quoted_and_firewall_is_private_only(self):
        self.assertEqual(d.ps_string("c:/it's mine/$file"), "'c:/it''s mine/$file'")
        with patch.object(d.sys, 'platform', 'win32'), patch.object(d, 'powershell', return_value=Mock(returncode=0)) as ps:
            d.configure_network({'port':18080,'discovery_port':49645}, Mock())
        script = ps.call_args.args[0]
        self.assertIn('-Profile Private -RemoteAddress LocalSubnet', script)
        self.assertNotIn('-Profile Any', script)
        self.assertTrue(ps.call_args.kwargs['elevated'])


if __name__ == '__main__':
    unittest.main()
