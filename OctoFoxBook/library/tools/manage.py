"""Start, check and stop this OctoFox installation without deleting its data."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys

from configure import configuration, write_configuration

ROOT = Path(__file__).resolve().parents[1]
SERVICES = ('database', 'booklore', 'library')


class LaunchError(Exception):
    pass


def json_rows(text):
    """Compose releases use either a JSON array or JSON Lines for ps."""
    if not text.strip():
        return []
    if text.lstrip().startswith('['):
        return json.loads(text)
    return [json.loads(line) for line in text.splitlines() if line.strip()]


class Installation:
    def __init__(self, root=ROOT, project='octofox-library'):
        self.root = Path(root).resolve()
        self.project = project
        self.docker = shutil.which('docker')
        # The checked-in Compose file and this directory's .env are always used,
        # even if the command was launched from elsewhere or COMPOSE_FILE is set.
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(('COMPOSE_', 'OCTOFOX_', 'BOOKLORE_'))
                    and k not in {'MARIADB_ROOT_PASSWORD', 'TZ'}}
        self.command = [self.docker or 'docker', 'compose', '--project-directory', str(self.root),
                        '--file', str(self.root / 'compose.yaml'), '--env-file', str(self.root / '.env'),
                        '--project-name', project, '--profile', 'speech']

    def run(self, args, description, *, compose=True, visible=False, timeout=30):
        command = (self.command if compose else [self.docker or 'docker']) + list(args)
        try:
            result = subprocess.run(command, cwd=self.root, env=self.env, text=True,
                                    encoding='utf-8', errors='replace', capture_output=not visible,
                                    timeout=timeout)
        except FileNotFoundError:
            raise LaunchError('Docker was not found. Install Docker Engine with Compose v2, or Docker Desktop.') from None
        except subprocess.TimeoutExpired:
            raise LaunchError(f'{description} timed out. Run status before retrying; existing data was retained.') from None
        except OSError:
            raise LaunchError(f'{description} could not run. Check access to Docker.') from None
        if result.returncode:
            # Captured config/engine diagnostics may contain environment values.
            # Do not copy them into an ordinary launcher report.
            raise LaunchError(f'{description} failed (exit {result.returncode}). Check Docker and run status. Data was retained.')
        return result.stdout or ''

    def prerequisites(self):
        version = self.run(['compose', 'version', '--short'], 'Compose check', compose=False).strip()
        match = re.match(r'v?(\d+)\.(\d+)', version)
        if not match or tuple(map(int, match.groups())) < (2, 20):
            raise LaunchError('Docker Compose 2.20 or newer is required.')
        engine = self.run(['info', '--format', '{{.OSType}}'], 'Docker engine check', compose=False).strip()
        if engine != 'linux':
            raise LaunchError('Use a Linux Docker engine. On Windows, enable Linux containers in Docker Desktop.')
        endpoint = self.env.get('DOCKER_HOST') if not self.env.get('DOCKER_CONTEXT') else None
        if not endpoint:
            endpoint = self.run(['context', 'inspect', '--format', '{{.Endpoints.docker.Host}}'],
                                'Docker context check', compose=False).strip()
        if not endpoint.startswith(('unix://', 'npipe://')):
            raise LaunchError('Run this launcher on the Docker host with a local Docker context. '
                              'Remote SSH/TCP Docker contexts are not supported by the host-port checks.')
        print(f'Docker is available; Compose {version}; Linux containers.')

    def guard_project(self):
        ids = self.run(['ps', '-aq', '--filter', f'label=com.docker.compose.project={self.project}'],
                       'Installation ownership check', compose=False).split()
        if not ids:
            return
        labels = self.run(['inspect', '--format', '{{json .Config.Labels}}', *ids],
                          'Installation ownership check', compose=False)
        expected = os.path.normcase(str(self.root / 'compose.yaml'))
        for row in json_rows(labels):
            files = (row or {}).get('com.docker.compose.project.config_files', '').split(',')
            if len(files) != 1 or os.path.normcase(os.path.abspath(files[0])) != expected:
                raise LaunchError('This Docker project belongs to another location or Compose configuration. '
                                  'Use its original folder, or choose a different --project for a separate installation.')

    def settings(self):
        if not (self.root / '.env').is_file():
            raise LaunchError('No settings yet. Run start to create them, or python tools/configure.py.')
        document = json.loads(self.run(['config', '--format', 'json'], 'Configuration validation'))
        environment = document['services']['library']['environment']
        bind = environment['OCTOFOX_PUBLISHED_BIND']
        port = int(environment['OCTOFOX_PUBLISHED_PORT'])
        origin = environment['OCTOFOX_ORIGIN']
        admin_port = int(document['services']['booklore']['ports'][0]['published'])
        configuration(origin, bind, port, admin_port)  # Share the existing validation contract.
        udp = next((int(p['published']) for p in document['services']['library'].get('ports', [])
                    if p.get('protocol') == 'udp'), None)
        return {'origin': origin, 'bind': bind, 'port': port, 'admin_port': admin_port, 'discovery_port': udp}

    def rows(self):
        return json_rows(self.run(['ps', '--all', '--format', 'json'], 'Service status'))

    def ports(self, settings, rows):
        entries = [('library', settings['bind'], settings['port'], 'tcp'),
                   ('booklore', '127.0.0.1', settings['admin_port'], 'tcp')]
        if settings.get('discovery_port'):
            entries.append(('library', settings['bind'], settings['discovery_port'], 'udp'))
        for service, address, port, protocol in entries:
            owned = any(row.get('Service') == service and row.get('State') == 'running'
                        and any(int(p.get('PublishedPort', 0)) == port and p.get('Protocol', 'tcp') == protocol for p in row.get('Publishers') or [])
                        for row in rows)
            if owned:
                continue
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM if protocol == 'udp' else socket.SOCK_STREAM) as probe:
                if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                try:
                    probe.bind((address, port))
                except OSError:
                    raise LaunchError(f'Port {port} cannot be used on {address}. '
                                      'Choose an available address/port in this installation\'s .env before starting.') from None

    def show_status(self, rows):
        indexed = {row.get('Service'): row for row in rows}
        for service in (*SERVICES, 'speech'):
            row = indexed.get(service)
            if row:
                print(f"{service}: {row.get('State', 'unknown')} / {row.get('Health') or 'no health result'}")
            else:
                print(f"{service}: {'not enabled' if service == 'speech' else 'not created'}")
        return all(indexed.get(s, {}).get('State') == 'running' and indexed[s].get('Health') == 'healthy'
                   for s in SERVICES) and ('speech' not in indexed or (
                       indexed['speech'].get('State') == 'running' and indexed['speech'].get('Health') == 'healthy'))

    def links(self, settings):
        print(f"Companion: {settings['origin']}/companion")
        print(f"Library:   {settings['origin']}")
        if settings['bind'] == '127.0.0.1':
            print('This installation listens on this computer only. Use an SSH tunnel for a remote server, '
                  'or configure a LAN address to connect from another device.')
        print('These are configured addresses; external access is checked separately in Companion.')

    def start(self, args):
        path = self.root / '.env'
        supplied = any(getattr(args, key) is not None for key in ('origin', 'bind', 'port', 'admin_port'))
        if path.exists() and supplied:
            raise LaunchError('Settings already exist and were preserved. Edit .env to change addresses or ports.')
        self.guard_project()
        if not path.exists():
            port = args.port if args.port is not None else 8080
            values = configuration(args.origin or f'http://localhost:{port}', args.bind or '0.0.0.0',
                                   port, args.admin_port if args.admin_port is not None else 8081)
            write_configuration(path, values)
            print('Created private .env settings. Keep this file with your data backup.')
        settings = self.settings()
        rows = self.rows()
        self.ports(settings, rows)
        services = list(SERVICES)
        if args.speech or any(row.get('Service') == 'speech' for row in rows):
            services.append('speech')
        print('Starting OctoFox. The first build can take several minutes.', flush=True)
        self.run(['up', '--detach', '--wait', '--wait-timeout', str(args.wait_timeout),
                  '--no-build' if args.no_build else '--build', *services],
                 'Startup', visible=True, timeout=None)
        if not self.show_status(self.rows()):
            raise LaunchError('Startup returned, but not all enabled services are healthy. Run status for details.')
        self.links(settings)
        print('Open the OctoFox Companion app on this computer or in the same local network. It finds this server and opens owner setup automatically.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', default='octofox-library', help='Docker project name; use the same name for every command')
    commands = parser.add_subparsers(dest='action', required=True)
    start = commands.add_parser('start', help='Create settings if absent, build and wait for readiness')
    start.add_argument('--speech', action='store_true', help='Enable server narration (existing speech containers are retained)')
    start.add_argument('--no-build', action='store_true', help='Reuse already built images')
    start.add_argument('--wait-timeout', type=int, default=300, help='Readiness timeout in seconds, after building')
    for flag in ('origin', 'bind', 'port', 'admin-port'):
        start.add_argument('--' + flag, type=int if 'port' in flag else str, help='Only used when creating a new .env')
    for name, help_text in [('check', 'Check Docker, settings and host ports without starting services'),
                            ('status', 'Show health of this installation'),
                            ('stop', 'Stop services while retaining volumes and settings'),
                            ('setup-key', 'Print the private initial setup key only when explicitly requested')]:
        commands.add_parser(name, help=help_text)
    args = parser.parse_args(argv)
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,62}', args.project):
        parser.error('Use a lowercase Docker project name (letters, digits, hyphens, underscores).')
    if args.action == 'start' and not 1 <= args.wait_timeout <= 3600:
        parser.error('--wait-timeout must be between 1 and 3600 seconds.')
    app = Installation(project=args.project)
    try:
        app.prerequisites()
        if args.action == 'start':
            app.start(args)
        else:
            app.guard_project()
            if args.action == 'check' and not (app.root / '.env').exists():
                print('Ready for a first start. No settings have been created yet.')
                return 0
            settings = app.settings()
            if args.action == 'check':
                app.ports(settings, app.rows())
                print('Settings and configured host ports are ready.')
                app.links(settings)
            elif args.action == 'status':
                healthy = app.show_status(app.rows())
                app.links(settings)
                return 0 if healthy else 1
            elif args.action == 'stop':
                app.run(['stop'], 'Stop', visible=True, timeout=120)
                print('Stopped. Books, accounts, reading positions and settings were retained.')
            elif args.action == 'setup-key':
                key = app.run(['exec', '-T', 'library', 'cat', '/data/companion-setup-key'], 'Setup key retrieval').strip()
                if not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', key):
                    raise LaunchError('The setup key could not be read. Check that the library is running.')
                print('Private initial setup key (enter it only in your Companion):')
                print(key)
        return 0
    except (LaunchError, ValueError, KeyError, OSError) as error:
        # Never include raw config/JSON/parser exceptions: they may contain secrets.
        print(str(error) if isinstance(error, LaunchError) else 'Invalid or unreadable settings. Check .env and compose.yaml.', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('Interrupted. Run status before retrying; existing data was retained.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    raise SystemExit(main())
