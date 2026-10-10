"""Install the OctoFox book server. The portable Companion browser is separate."""
import argparse
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import webbrowser
import dependencies

HERE = Path(__file__).resolve().parent
SOURCE = HERE / 'library' if getattr(sys, 'frozen', False) else HERE.parent / 'library'
sys.path.insert(0, str(SOURCE / 'tools'))
from manage import Installation, LaunchError  # noqa: E402
from configure import configuration  # noqa: E402

MARKER = '.octofox-install.json'
ROOT_FILES = ('compose.yaml', 'Dockerfile', '.dockerignore', 'pyproject.toml', 'README.md', 'THIRD_PARTY.md')
RUNTIME_TOOLS = ('configure.py', 'manage.py')


def payload_files(source=SOURCE):
    """Only server code and build inputs; never copy a developer's .env or data."""
    result = [source / name for name in ROOT_FILES]
    # Developer verification fixtures are source-only, not installation payload.
    result += [source / 'tools' / name for name in RUNTIME_TOOLS]
    for name in ('src', 'speech'):
        result += [p for p in (source / name).rglob('*') if p.is_file()
                   and not any(x.startswith('.') or x == '__pycache__' or x.endswith('.egg-info')
                               for x in p.relative_to(source).parts)
                   and p.suffix not in ('.pyc', '.pyo')]
    if any(p.is_symlink() for p in result):
        raise LaunchError('The server package contains a symbolic link. Use an original package.')
    return sorted(result)


def payload_hash(source=SOURCE):
    digest = hashlib.sha256()
    for p in payload_files(source):
        digest.update(p.relative_to(source).as_posix().encode() + b'\0')
        digest.update(p.read_bytes())
    return digest.hexdigest()


def destination(value):
    if not isinstance(value, str) or not value.strip() or '\0' in value:
        raise LaunchError('Choose a full installation folder path.')
    p = Path(value).expanduser()
    if not p.is_absolute() or p == Path(p.anchor):
        raise LaunchError('Choose a full folder path, not the disk root.')
    if any(q.is_symlink() or (hasattr(q, 'is_junction') and q.is_junction()) for q in (p, *p.parents)):
        raise LaunchError('Choose a folder outside symbolic links and junctions.')
    return p.resolve()


def default_directory():
    if sys.platform == 'win32':
        base = Path(os.environ.get('LOCALAPPDATA', str(Path.home() / 'AppData/Local')))
    elif sys.platform == 'darwin':
        base = Path.home() / 'Library/Application Support'
    else:
        base = Path.home() / '.local/share'
    return str(base / 'OctoFox/server')


def available_port(start, exclude=()):
    for port in range(start, min(start + 100, 65536)):
        if port in exclude:
            continue
        with socket.socket() as probe:
            if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            try:
                probe.bind(('0.0.0.0', port))
                return port
            except OSError:
                pass
    raise LaunchError('No available library port was found. Choose a different port under Advanced settings.')


def installed_marker(target):
    try:
        value = json.loads((target / MARKER).read_text(encoding='utf-8'))
        if value['product'] != 'octofox-server' or value['schema'] != 1:
            raise ValueError()
        expected = 'octofox-' + hashlib.sha256(os.path.normcase(str(target)).encode()).hexdigest()[:12]
        if value['project'] != expected:
            raise ValueError()
        return value
    except (OSError, ValueError, KeyError, TypeError):
        raise LaunchError('This folder is not an installation created here by this installer. Choose an empty folder.') from None


def validate_target(target, source=SOURCE):
    if target.exists():
        if not target.is_dir():
            raise LaunchError('The installation path is a file. Choose a folder.')
        if any(target.iterdir()):
            if any((target / name).is_symlink() for name in (MARKER, '.env')):
                raise LaunchError('Installation settings contain symbolic links. Choose the original installation folder.')
            marker = installed_marker(target)
            if marker.get('payload') != payload_hash(source):
                raise LaunchError('This folder contains a different server version. Use its original installer; updates are a separate operation.')
            for p in payload_files(source):
                dest = target / p.relative_to(source)
                if any(q.is_symlink() or (hasattr(q, 'is_junction') and q.is_junction()) for q in (dest, *dest.parents)):
                    raise LaunchError('Installed files contain symbolic links. Use the original installation.')
                if not dest.is_file() or hashlib.sha256(dest.read_bytes()).digest() != hashlib.sha256(p.read_bytes()).digest():
                    raise LaunchError('Installed server files have changed. Existing files were preserved.')
            return marker
    return None


def prepare(target, source=SOURCE):
    marker = validate_target(target, source)
    if marker:
        return marker
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.octofox-install-', dir=target.parent))
    try:
        for p in payload_files(source):
            dest = staging / p.relative_to(source)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, dest)
        marker = {'product': 'octofox-server', 'schema': 1, 'payload': payload_hash(source),
                  'project': 'octofox-' + hashlib.sha256(os.path.normcase(str(target)).encode()).hexdigest()[:12]}
        (staging / MARKER).write_text(json.dumps(marker, indent=2) + '\n', encoding='utf-8')
        # Only an empty destination may be replaced. No recursive removal of user folders.
        if target.exists():
            target.rmdir()
        staging.rename(target)
        return marker
    finally:
        if staging.exists():
            shutil.rmtree(staging)


class QuietInstallation(Installation):
    def __init__(self, root, project, progress):
        super().__init__(root, project)
        self.progress = progress

    def run(self, args, description, **kwargs):
        if description == 'Startup':
            self.progress('Installing and starting services', 'Downloading and building the server. The first installation can take several minutes.')
        command = (self.command[1:] if kwargs.get('compose', True) else []) + list(args)
        try:
            result = subprocess.run(dependencies.docker_command(command), cwd=self.root, env=self.env,
                                    text=True, encoding='utf-8', errors='replace', capture_output=True,
                                    timeout=kwargs.get('timeout', 30),
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except (OSError, subprocess.TimeoutExpired):
            raise LaunchError(description + ' could not finish. Check that Docker is running, then retry. Existing data was retained.') from None
        if result.returncode:
            raise LaunchError(description + ' failed. Check your Internet connection and Docker, then retry. Existing data was retained.')
        return result.stdout


class Installer:
    def __init__(self, source=SOURCE, factory=QuietInstallation, runtime=dependencies, headless=False, choices=None):
        self.source, self.factory = source, factory
        self.runtime, self.headless, self.choices = runtime, headless, choices
        self.lock = threading.Lock()
        self.state = {'busy': False, 'phase': 'Ready to check', 'detail': '', 'result': None, 'error': None}

    def snapshot(self):
        with self.lock:
            return dict(self.state)

    def progress(self, phase, detail=''):
        with self.lock:
            self.state.update(phase=phase, detail=detail)

    def submit(self, action, options):
        if action not in ('check', 'install'):
            raise LaunchError('Unknown installer action.')
        with self.lock:
            if self.state['busy']:
                raise LaunchError('An operation is already running. Wait for its result.')
            self.state.update(busy=True, error=None, result=None)
        thread = threading.Thread(target=self.work, args=(action, options), daemon=False)
        thread.start()
        return thread

    def work(self, action, options):
        try:
            target = destination(options.get('directory'))
            speech = options.get('speech', True)
            if type(speech) is not bool:
                raise LaunchError('Choose whether to enable narration.')
            port, admin_port = options.get('port', 8080), options.get('adminPort', 8081)
            if type(port) is not int or type(admin_port) is not int:
                raise LaunchError('Ports must be whole numbers.')
            configuration(f'http://localhost:{port}', '0.0.0.0', port, admin_port)
            self.progress('Checking this computer', 'Checking Docker, the destination and network ports.')
            marker = validate_target(target, self.source)
            if action == 'install':
                if self.choices:
                    self.choices.parent.mkdir(parents=True, exist_ok=True)
                    with open(self.choices, 'w', encoding='utf-8') as output:
                        json.dump(dict(directory=str(target), speech=speech, port=port, adminPort=admin_port), output)
                    self.choices.chmod(0o600)
                self.runtime.ensure(self.progress, self.headless)
            elif not self.runtime.ready():
                self.progress('System components will be prepared', 'Choose Install server. Required downloads and system setup will run automatically; confirm any system permission or Docker terms prompts.')
                with self.lock:
                    self.state['result'] = {'needsRuntime': True, 'directory': str(target)}
                return
            # Prerequisites run in the bundled folder, before creating the destination.
            app = self.factory(self.source, 'octofox-check', self.progress)
            with contextlib.redirect_stdout(io.StringIO()):
                app.prerequisites()
            arch = app.run(['info', '--format', '{{.Architecture}}'], 'Docker architecture check', compose=False).strip()
            if arch not in ('x86_64', 'amd64'):
                raise LaunchError('This server package currently requires an x64 Linux Docker engine. The Companion browser can run on other computers.')
            project = marker['project'] if marker else 'octofox-' + hashlib.sha256(os.path.normcase(str(target)).encode()).hexdigest()[:12]
            # Docker commands need an existing cwd, but project ownership is checked after preparation.
            settings = {'origin': f'http://localhost:{port}', 'bind': '0.0.0.0',
                        'port': port, 'admin_port': admin_port, 'discovery_port': 49645}
            rows = []
            if marker:
                app = self.factory(target, project, self.progress)
                app.guard_project()
                if (target / '.env').exists():
                    settings, rows = app.settings(), app.rows()
            if not marker or not (target / '.env').exists():
                if port == 8080:
                    port = available_port(8080, (admin_port,))
                if admin_port == 8081:
                    admin_port = available_port(8081, (port,))
                settings.update(port=port, admin_port=admin_port, origin=f'http://localhost:{port}')
            app.ports(settings, rows)
            if action == 'check':
                self.progress('Ready to install' if not marker else 'Ready to start', f"Docker is ready. The library will use port {settings['port']}.")
                result = {'directory': str(target), 'existing': bool(marker), 'project': project}
            else:
                self.progress('Preparing server files', 'Keeping settings and existing data in place.')
                marker = prepare(target, self.source)
                app = self.factory(target, marker['project'], self.progress)
                existing_env = (target / '.env').exists()
                self.runtime.configure_network(settings, self.progress)
                args = SimpleNamespace(origin=None, bind=None, port=None if existing_env else port,
                                       admin_port=None if existing_env else admin_port, speech=speech,
                                       no_build=False, wait_timeout=600)
                with contextlib.redirect_stdout(io.StringIO()):
                    app.start(args)
                result = {'directory': str(target), 'project': marker['project'],
                          'url': app.settings()['origin'], 'existing': bool(existing_env)}
                self.progress('Your server is ready', 'Open the portable Companion browser on this computer or another computer on the same network. It will find this server.')
            with self.lock:
                self.state['result'] = result
        except Exception as error:
            # Never forward Docker/configuration output or unexpected exception details to HTTP.
            message = str(error) if isinstance(error, (LaunchError, dependencies.DependencyError)) else 'The installation could not complete. Check the folder permissions and port settings, then retry. Existing data was retained.'
            with self.lock:
                self.state.update(error=message, phase='Needs attention', detail='')
        finally:
            with self.lock:
                self.state['busy'] = False


def make_server(installer, port=0):
    token = secrets.token_urlsafe(32)

    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(10)

        def log_message(self, *args):
            pass

        def send(self, status, data, content_type='application/json'):
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Referrer-Policy', 'no-referrer')
            self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self'; script-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
            self.end_headers()
            self.wfile.write(data)

        def valid(self, auth=False):
            if self.headers.get('Host') != self.server.address or self.headers.get('Origin') not in (None, self.server.origin):
                return False
            return not auth or secrets.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + token)

        def do_GET(self):
            self.server.touched = time.monotonic()
            if not self.valid(self.path.startswith('/api/')):
                return self.send(403, b'{}')
            if self.path == '/api/state':
                return self.send(200, json.dumps(installer.snapshot()).encode())
            if self.path == '/api/defaults':
                defaults = {'directory': default_directory(), 'port': 8080, 'adminPort': 8081, 'speech': True}
                if installer.choices and installer.choices.exists():
                    try:
                        saved = json.loads(installer.choices.read_text(encoding='utf-8'))
                        defaults.update({k: saved[k] for k in defaults if k in saved})
                    except (ValueError, OSError, TypeError):
                        pass
                return self.send(200, json.dumps(defaults).encode())
            assets = {'/': ('index.html', 'text/html; charset=utf-8'), '/app.js': ('app.js', 'text/javascript; charset=utf-8'), '/style.css': ('style.css', 'text/css; charset=utf-8')}
            if self.path not in assets:
                return self.send(404, b'{}')
            name, kind = assets[self.path]
            self.send(200, (HERE / 'web' / name).read_bytes(), kind)

        def do_POST(self):
            self.server.touched = time.monotonic()
            if not self.valid(True) or self.headers.get('Origin') != self.server.origin:
                return self.send(403, b'{}')
            if self.headers.get('Content-Type') != 'application/json':
                return self.send(415, b'{}')
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 16384:
                    return self.send(413, b'{}')
                options = json.loads(self.rfile.read(length))
                if not isinstance(options, dict):
                    return self.send(400, b'{}')
                if self.path == '/api/close':
                    if installer.snapshot()['busy']:
                        return self.send(409, b'{}')
                    self.send(200, b'{}')
                    threading.Thread(target=self.server.shutdown, daemon=True).start()
                    return
                if self.path not in ('/api/check', '/api/install'):
                    return self.send(404, b'{}')
                installer.submit(self.path.rsplit('/', 1)[-1], options)
                self.send(202, b'{}')
            except (ValueError, LaunchError):
                self.send(409, b'{"error":"Another operation is running, or the request is invalid."}')

    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.address = f'127.0.0.1:{server.server_port}'
    server.origin = 'http://' + server.address
    server.token = token
    server.touched = time.monotonic()
    return server


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headless', action='store_true', help='Install on this host without opening a browser')
    parser.add_argument('--check', action='store_true', help='Only check prerequisites in headless mode')
    parser.add_argument('--directory', default=default_directory())
    parser.add_argument('--no-speech', action='store_true')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--admin-port', type=int, default=8081)
    parser.add_argument('--no-open', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--session-file', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    installer = Installer(headless=args.headless, choices=Path(default_directory()).parent / 'installer-choices.json')
    if args.headless:
        options = {'directory': args.directory, 'speech': not args.no_speech, 'port': args.port, 'adminPort': args.admin_port}
        worker = installer.submit('check' if args.check else 'install', options)
        last = None
        while worker.is_alive():
            state = installer.snapshot()
            if state['phase'] != last:
                print(state['phase'], flush=True)
                last = state['phase']
            worker.join(1)
        state = installer.snapshot()
        print(json.dumps(state), flush=True)
        return 1 if state['error'] else 0
    server = make_server(installer)
    url = server.origin + '/#' + server.token
    if args.session_file:
        fd = os.open(args.session_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8') as output:
            json.dump({'url': url}, output)
    if not args.no_open:
        webbrowser.open(url)
    # An abandoned idle installer exits; closing a tab during installation never kills Docker.
    def idle():
        while True:
            time.sleep(10)
            if time.monotonic() - server.touched > 300 and not installer.snapshot()['busy']:
                server.shutdown()
                return
    threading.Thread(target=idle, daemon=True).start()
    try:
        server.serve_forever(poll_interval=.2)
    finally:
        server.server_close()
        if args.session_file:
            args.session_file.unlink(missing_ok=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
