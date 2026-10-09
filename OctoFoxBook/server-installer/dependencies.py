"""Native prerequisite installation; commands are lists, never user-provided shell text."""
import base64
import getpass
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request


class DependencyError(Exception):
    pass


def run(command, *, timeout=60, check=True):
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                timeout=timeout, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (OSError, subprocess.TimeoutExpired):
        raise DependencyError('A system component did not respond. Retry after any system installation has finished.') from None
    if check and result.returncode:
        raise DependencyError('System setup did not finish. If a permission prompt was cancelled, choose Install again. No library data was removed.')
    return result


def powershell(script, elevated=False):
    encoded = base64.b64encode(script.encode('utf-16le')).decode()
    if elevated:
        script = "$ErrorActionPreference='Stop'; $p=Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -Wait -PassThru -ArgumentList '-NoProfile','-NonInteractive','-EncodedCommand','" + encoded + "'; exit $p.ExitCode"
        encoded = base64.b64encode(script.encode('utf-16le')).decode()
    return run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded], timeout=1800, check=False)


def ps_string(value):
    return "'" + str(value).replace("'", "''") + "'"


def refresh_path():
    candidates = []
    if sys.platform == 'win32':
        for root in (Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs/DockerDesktop',
                     Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Docker/Docker'):
            candidates.append(root / 'resources/bin')
    elif sys.platform == 'darwin':
        candidates = [Path.home() / '.docker/bin', Path('/Applications/Docker.app/Contents/Resources/bin')]
    existing = os.environ.get('PATH', '').split(os.pathsep)
    os.environ['PATH'] = os.pathsep.join([str(p) for p in candidates if p.is_dir() and str(p) not in existing] + existing)


def docker_command(args):
    command = [shutil.which('docker') or 'docker', *args]
    if sys.platform.startswith('linux') and os.geteuid() != 0:
        import grp
        try:
            group = grp.getgrnam('docker')
            if group.gr_gid not in os.getgroups() and getpass.getuser() in group.gr_mem:
                return ['sg', 'docker', '-c', shlex.join(command)]
        except KeyError:
            pass
    return command


def ready():
    refresh_path()
    if not shutil.which('docker'):
        return False
    engine = run(docker_command(['info', '--format', '{{.OSType}}']), check=False)
    version = run(docker_command(['compose', 'version', '--short']), check=False)
    builder = run(docker_command(['buildx', 'version']), check=False)
    match = re.match(r'v?(\d+)\.(\d+)', version.stdout.strip())
    return engine.returncode == version.returncode == builder.returncode == 0 and engine.stdout.strip() == 'linux' and bool(match) and tuple(map(int, match.groups())) >= (2, 20)


class OfficialRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        from urllib.parse import urlsplit
        url = urlsplit(newurl)
        if url.scheme != 'https' or url.hostname != 'desktop.docker.com':
            raise DependencyError('Docker download redirected outside its official download host.')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(url, dest, progress):
    progress('Downloading Docker', 'Getting the required runtime from Docker’s official download server.')
    try:
        with urllib.request.build_opener(OfficialRedirect()).open(url, timeout=60) as response, open(dest, 'wb') as output:
            total = int(response.headers.get('Content-Length', 0))
            count = 0
            last = -1
            while chunk := response.read(1024 * 1024):
                count += len(chunk)
                if count > 3 * 1024 ** 3:
                    raise DependencyError('The Docker download was larger than expected. Please retry.')
                output.write(chunk)
                percent = int(count * 100 / total) if total else count // 1024 ** 2
                if percent != last:
                    progress('Downloading Docker', f'{percent}% downloaded' if total else f'{percent} MB downloaded')
                    last = percent
    except (OSError, ValueError):
        raise DependencyError('The download was interrupted. Check your Internet connection and choose Install again.') from None


def install_windows(progress):
    refresh_path()
    local = Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs/DockerDesktop/Docker Desktop.exe'
    shared = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Docker/Docker/Docker Desktop.exe'
    desktop = next((p for p in (local, shared) if p.is_file()), None)
    if desktop is None:
        progress('Preparing Windows', 'Installing or updating Windows Subsystem for Linux. Confirm the Windows permission prompt if it appears.')
        # No Linux distribution is needed: Docker provisions its own environment.
        wsl_version = run(['wsl.exe', '--version'], check=False)
        wsl = powershell("& wsl.exe --install --no-distribution; exit $LASTEXITCODE", elevated=True) if wsl_version.returncode else wsl_version
        if wsl.returncode in (3010, 1641):
            raise DependencyError('Windows needs a restart to finish system setup. Restart this computer, then open this installer again. Your selected folder is remembered.')
        if wsl.returncode:
            raise DependencyError('Windows could not prepare WSL. Confirm the system permission prompt; if Windows requests a restart, restart and open this installer again.')
        update = powershell("& wsl.exe --update; exit $LASTEXITCODE", elevated=True)
        if update.returncode:
            raise DependencyError('Windows could not finish updating WSL. Restart if requested, then retry the installation.')
        with tempfile.TemporaryDirectory(prefix='octofox-docker-') as temp:
            package = Path(temp) / 'Docker Desktop Installer.exe'
            download('https://desktop.docker.com/win/main/amd64/Docker%20Desktop%20Installer.exe', package, progress)
            signature = powershell("$s=Get-AuthenticodeSignature -LiteralPath " + ps_string(package) + "; if($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch '(?:^|, )O=\"?Docker Inc[.,]') {exit 1}")
            if signature.returncode:
                raise DependencyError('The downloaded Docker installer did not pass its publisher signature check. Installation stopped.')
            progress('Installing Docker', 'Installing the server runtime. This may take a few minutes.')
            result = run([str(package), 'install', '--user', '--quiet', '--backend=wsl-2'], timeout=1800, check=False)
            if result.returncode in (3010, 1641):
                raise DependencyError('Windows needs a restart. Restart, then open this installer again to continue.')
            if result.returncode:
                raise DependencyError('Docker installation could not finish. Restart if Windows requests it, then retry.')
        desktop = next((p for p in (local, shared) if p.is_file()), None)
    if not desktop:
        raise DependencyError('Docker was installed but its application could not be found. Reopen the installer after signing in again.')
    progress('Starting Docker', 'Docker may ask you to accept its terms. Complete that prompt; installation continues automatically here.')
    subprocess.Popen([str(desktop)], creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def install_mac(progress):
    app = Path('/Applications/Docker.app')
    if not app.exists():
        arch = 'arm64' if platform.machine() == 'arm64' else 'amd64'
        with tempfile.TemporaryDirectory(prefix='octofox-docker-') as temp:
            package, mount = Path(temp) / 'Docker.dmg', Path(temp) / 'volume'
            download(f'https://desktop.docker.com/mac/main/{arch}/Docker.dmg', package, progress)
            run(['hdiutil', 'attach', str(package), '-nobrowse', '-readonly', '-mountpoint', str(mount)], timeout=180)
            try:
                bundled = mount / 'Docker.app'
                run(['codesign', '--verify', '--deep', '--strict', str(bundled)], timeout=180)
                details = run(['codesign', '-dv', '--verbose=4', str(bundled)])
                if 'TeamIdentifier=9BNSXJN65R' not in details.stderr:
                    raise DependencyError('Docker publisher verification failed. Installation stopped.')
                progress('Installing Docker', 'Confirm the macOS administrator prompt to install the server runtime.')
                command = shlex.join([str(bundled / 'Contents/MacOS/install'), '--user=' + getpass.getuser()])
                run(['osascript', '-e', 'do shell script ' + json.dumps(command) + ' with administrator privileges'], timeout=1800)
            finally:
                run(['hdiutil', 'detach', str(mount)], check=False)
    progress('Starting Docker', 'Docker may ask you to accept its terms. Complete that prompt; installation continues automatically here.')
    run(['open', '-a', str(app)])


LINUX_SCRIPT = r'''#!/bin/sh
set -eu
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
case "$ID" in ubuntu|debian) ;; *) exit 41 ;; esac
if ! command -v docker >/dev/null 2>&1 || ! docker compose version >/dev/null 2>&1 || ! docker buildx version >/dev/null 2>&1 || ! dpkg --compare-versions "$(docker compose version --short | sed 's/^v//')" ge 2.20; then
  apt-get update
  apt-get install -y --no-remove ca-certificates curl
  install -m 0755 -d /etc/apt/keyrings
  # Use a separate source file, preserving existing system source files.
  if ! grep -rqs 'download.docker.com/linux/' /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null; then
    curl --proto '=https' --tlsv1.2 -fsS "https://download.docker.com/linux/$ID/gpg" -o /etc/apt/keyrings/octofox-docker.asc
    chmod 0644 /etc/apt/keyrings/octofox-docker.asc
    codename="${UBUNTU_CODENAME:-$VERSION_CODENAME}"
    arch="$(dpkg --print-architecture)"
    printf 'Types: deb\nURIs: https://download.docker.com/linux/%s\nSuites: %s\nComponents: stable\nArchitectures: %s\nSigned-By: /etc/apt/keyrings/octofox-docker.asc\n' "$ID" "$codename" "$arch" > /etc/apt/sources.list.d/octofox-docker.sources
    apt-get update
  fi
  if command -v docker >/dev/null 2>&1; then
    apt-get install -y --no-remove docker-buildx-plugin docker-compose-plugin
  else
    apt-get install -y --no-remove docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
  fi
fi
systemctl enable --now docker
if [ "$1" != root ]; then usermod -aG docker -- "$1"; fi
'''


def install_linux(progress, headless=False):
    try:
        release = platform.freedesktop_os_release()
    except OSError:
        release = {}
    if release.get('ID') not in ('ubuntu', 'debian'):
        raise DependencyError('Automatic runtime installation supports Ubuntu and Debian. This system needs an existing Docker Engine with Compose 2.20 or newer.')
    progress('Installing Docker', 'Installing the official Docker Engine and Compose packages. Confirm the system administrator prompt if it appears.')
    with tempfile.TemporaryDirectory(prefix='octofox-runtime-') as temp:
        script = Path(temp) / 'install-runtime.sh'
        script.write_text(LINUX_SCRIPT, encoding='utf-8')
        script.chmod(0o700)
        command = ['/bin/sh', str(script), getpass.getuser()]
        if os.geteuid() != 0:
            if headless:
                # In an actual terminal sudo owns its native password prompt, never the web UI.
                result = subprocess.run(['sudo', *command], timeout=1800)
                if result.returncode:
                    raise DependencyError('System package installation did not finish. Retry after resolving any package-manager or permission prompt.')
                return
            if not shutil.which('pkexec'):
                raise DependencyError('This Linux desktop has no system permission dialog. Run the installer with --headless in a terminal to use sudo.')
            command = ['pkexec', *command]
        run(command, timeout=1800)


def ensure(progress, headless=False):
    if ready():
        return
    # Never repoint a user's remote Docker context or change an existing engine's container mode.
    if os.environ.get('DOCKER_HOST', '').startswith(('ssh:', 'tcp:')) or os.environ.get('DOCKER_CONTEXT'):
        raise DependencyError('Use a local Docker context on the computer that will host your books.')
    if shutil.which('docker'):
        context = run(['docker', 'context', 'inspect', '--format', '{{.Endpoints.docker.Host}}'], check=False)
        if context.returncode == 0 and not context.stdout.strip().startswith(('unix://', 'npipe://')):
            raise DependencyError('The selected Docker context points to another computer. Select a local context before installation.')
        engine = run(docker_command(['info', '--format', '{{.OSType}}']), check=False)
        if engine.returncode == 0 and engine.stdout.strip() != 'linux':
            raise DependencyError('Docker is using Windows containers. Switch Docker Desktop to Linux containers, then retry.')
    if platform.machine().lower() not in ('amd64', 'x86_64'):
        raise DependencyError('This server package currently requires an x64 computer. The portable Companion browser can run on Apple Silicon too.')
    if sys.platform == 'win32':
        install_windows(progress)
    elif sys.platform == 'darwin':
        install_mac(progress)
    elif sys.platform.startswith('linux'):
        install_linux(progress, headless)
    else:
        raise DependencyError('This operating system is not supported for automatic server installation.')
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        if ready():
            return
        time.sleep(3)
    raise DependencyError('The runtime is installed but has not started yet. Finish any Docker terms or system restart prompt, then choose Install again.')


def configure_network(settings, progress):
    if sys.platform != 'win32':
        return
    port, udp = int(settings['port']), int(settings.get('discovery_port') or 49645)
    progress('Connecting your local network', 'Allowing the library on private local networks. Confirm the Windows permission prompt if it appears.')
    # No public-profile or Internet-facing rule. Rule names are owned by this product.
    script = "$ErrorActionPreference='Stop'; "
    for protocol, value in (('TCP', port), ('UDP', udp)):
        name = f'OctoFox-Server-{protocol}-{value}'
        script += (f"if(-not(Get-NetFirewallRule -Name '{name}' -ErrorAction SilentlyContinue)) {{"
                   f"New-NetFirewallRule -Name '{name}' -DisplayName 'OctoFox server {protocol} {value}' "
                   f"-Direction Inbound -Action Allow -Protocol {protocol} -LocalPort {value} -Profile Private -RemoteAddress LocalSubnet | Out-Null}}; ")
    result = powershell(script, elevated=True)
    if result.returncode:
        raise DependencyError('Windows did not allow the local-network rule. Confirm the permission prompt and retry to finish setup.')
