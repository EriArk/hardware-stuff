"""Launcher safety and lifecycle checks; no Docker daemon or private data required."""
from contextlib import redirect_stdout, redirect_stderr
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import manage
sys.path.pop(0)


def options(**kw):
    return SimpleNamespace(origin=None, bind=None, port=None, admin_port=None,
                           speech=kw.pop('speech', False), no_build=False, wait_timeout=300, **kw)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = manage.Installation(self.root, 'test-octofox')
        self.output = io.StringIO()
        self.context = redirect_stdout(self.output)
        self.context.__enter__()
        self.addCleanup(self.context.__exit__, None, None, None)

    def test_first_start_creates_private_settings_waits_and_never_prints_secrets(self):
        calls = []
        def run(args, *a, **kw):
            calls.append(args)
            return ''
        with patch.object(self.app, 'guard_project'), patch.object(self.app, 'settings', return_value={
            'origin': 'http://localhost:8080', 'bind': '127.0.0.1'}), patch.object(self.app, 'ports'), \
                patch.object(self.app, 'rows', return_value=[]), patch.object(self.app, 'show_status', return_value=True), \
                patch.object(self.app, 'run', side_effect=run):
            self.app.start(options())
        values = dict(line.split('=', 1) for line in (self.root / '.env').read_text().splitlines())
        self.assertNotEqual(values['BOOKLORE_DB_PASSWORD'], values['MARIADB_ROOT_PASSWORD'])
        self.assertNotIn(values['BOOKLORE_DB_PASSWORD'], self.output.getvalue())
        self.assertIn('--wait', calls[0])
        self.assertNotIn('speech', calls[0])
        self.assertIn('/companion', self.output.getvalue())
        if os.name != 'nt':
            self.assertEqual((self.root / '.env').stat().st_mode & 0o777, 0o600)

    def test_restart_preserves_settings_and_previously_enabled_narration(self):
        path = self.root / '.env'
        path.write_bytes(b'BOOKLORE_DB_PASSWORD=keep-this\n')
        with patch.object(self.app, 'guard_project'), patch.object(self.app, 'settings', return_value={
            'origin': 'http://localhost:8080', 'bind': '0.0.0.0'}), patch.object(self.app, 'ports'), \
                patch.object(self.app, 'rows', return_value=[{'Service':'speech', 'State':'exited'}]), \
                patch.object(self.app, 'show_status', return_value=True), patch.object(self.app, 'run') as run:
            self.app.start(options())
        self.assertIn('speech', run.call_args.args[0])
        self.assertEqual(path.read_bytes(), b'BOOKLORE_DB_PASSWORD=keep-this\n')
        args = options(); args.port = 8090
        with self.assertRaisesRegex(manage.LaunchError, 'preserved'):
            self.app.start(args)
        self.assertEqual(path.read_bytes(), b'BOOKLORE_DB_PASSWORD=keep-this\n')

    def test_occupied_port_fails_before_build_or_start(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); listener.listen()
            port = listener.getsockname()[1]
            settings = dict(bind='127.0.0.1', port=port, admin_port=port)
            with self.assertRaisesRegex(manage.LaunchError, f'Port {port}'):
                self.app.ports(settings, [])
            # The same installation may restart containers owning these ports.
            rows = [{'Service':s, 'State':'running', 'Publishers':[{'PublishedPort':port}]}
                    for s in ('library', 'booklore')]
            self.app.ports(settings, rows)

    def test_other_installation_with_same_project_is_never_modified(self):
        labels = {'com.docker.compose.project.config_files': str(self.root.parent / 'other' / 'compose.yaml')}
        with patch.object(self.app, 'run', side_effect=['id123', json.dumps(labels)]):
            with self.assertRaisesRegex(manage.LaunchError, 'another location'):
                self.app.guard_project()
        labels['com.docker.compose.project.config_files'] = str(self.root / 'compose.yaml')
        with patch.object(self.app, 'run', side_effect=['id123', json.dumps(labels)]):
            self.app.guard_project()

    def test_service_status_requires_health_not_just_running(self):
        rows = [{'Service':s, 'State':'running', 'Health':'healthy'} for s in manage.SERVICES]
        self.assertTrue(self.app.show_status(rows))
        self.assertFalse(self.app.show_status(rows[:-1]))
        self.assertFalse(self.app.show_status(rows + [{'Service':'speech', 'State':'exited'}]))
        rows[1]['Health'] = 'starting'
        self.assertFalse(self.app.show_status(rows))

    def test_both_compose_json_formats(self):
        rows = [{'Service':'library'}, {'Service':'speech'}]
        self.assertEqual(manage.json_rows(json.dumps(rows)), rows)
        self.assertEqual(manage.json_rows('\n'.join(map(json.dumps, rows))), rows)
        self.assertEqual(manage.json_rows(''), [])

    def test_environment_cannot_redirect_configuration_or_override_private_settings(self):
        with patch.dict(os.environ, {'COMPOSE_FILE':'other.yaml', 'COMPOSE_PROFILES':'unexpected',
                                     'BOOKLORE_DB_PASSWORD':'wrong', 'OCTOFOX_ORIGIN':'http://wrong',
                                     'DOCKER_CONTEXT':'desktop-linux'}):
            app = manage.Installation(self.root)
        for key in ('COMPOSE_FILE', 'COMPOSE_PROFILES', 'BOOKLORE_DB_PASSWORD', 'OCTOFOX_ORIGIN'):
            self.assertNotIn(key, app.env)
        self.assertEqual(app.env['DOCKER_CONTEXT'], 'desktop-linux')
        self.assertIn(str(self.root / '.env'), app.command)
        self.assertIn(str(self.root / 'compose.yaml'), app.command)

    def test_captured_docker_failures_do_not_expose_config_values(self):
        result = subprocess.CompletedProcess([], 1, stdout='password=secret', stderr='password=secret')
        with patch.object(manage.subprocess, 'run', return_value=result):
            with self.assertRaises(manage.LaunchError) as caught:
                self.app.run(['config'], 'Configuration validation')
        self.assertNotIn('secret', str(caught.exception))

    def test_missing_docker_old_compose_and_remote_context_fail_with_actionable_errors(self):
        with patch.object(manage.subprocess, 'run', side_effect=FileNotFoundError):
            with self.assertRaisesRegex(manage.LaunchError, 'Install Docker'):
                self.app.prerequisites()
        with patch.object(self.app, 'run', return_value='2.12.0'):
            with self.assertRaisesRegex(manage.LaunchError, '2.20'):
                self.app.prerequisites()
        self.app.env.pop('DOCKER_HOST', None)
        with patch.object(self.app, 'run', side_effect=['2.40.0', 'linux', 'ssh://another-server']):
            with self.assertRaisesRegex(manage.LaunchError, 'local Docker context'):
                self.app.prerequisites()

    def test_stop_retains_volumes_and_setup_key_is_only_explicit(self):
        with patch.object(manage, 'Installation', return_value=self.app), \
                patch.object(self.app, 'prerequisites'), patch.object(self.app, 'guard_project'), \
                patch.object(self.app, 'settings', return_value={}), patch.object(self.app, 'run') as run:
            self.assertEqual(manage.main(['--project', 'test-octofox', 'stop']), 0)
            self.assertEqual(run.call_args.args[0], ['stop'])
            run.return_value = 'x' * 43
            self.assertEqual(manage.main(['setup-key']), 0)
        self.assertIn('x' * 43, self.output.getvalue())

    def test_wrong_project_and_timeout_rejected_before_any_docker_call(self):
        with patch.object(manage, 'Installation') as create, redirect_stderr(io.StringIO()):
            for args in [['--project', '../oops', 'stop'], ['start', '--wait-timeout', '0']]:
                with self.assertRaises(SystemExit):
                    manage.main(args)
            create.assert_not_called()

    def test_config_failure_leaves_existing_credentials_unchanged(self):
        path = self.root / '.env'; path.write_text('BOOKLORE_DB_PASSWORD=original\n')
        with patch.object(self.app, 'guard_project'), patch.object(self.app, 'settings', side_effect=manage.LaunchError('Invalid settings')):
            with self.assertRaises(manage.LaunchError):
                self.app.start(options())
        self.assertEqual(path.read_text(), 'BOOKLORE_DB_PASSWORD=original\n')


if __name__ == '__main__':
    unittest.main()
