"""Build the standalone server installer on its target OS. No GitHub Actions needed."""
from pathlib import Path
import shutil
import subprocess
import sys

from installer import HERE, SOURCE, payload_files


def main():
    build = HERE / 'build'
    payload = build / 'payload/library'
    if payload.exists():
        # Only the fixed generated payload underneath this build directory is removed.
        assert payload.resolve().is_relative_to(build.resolve())
        shutil.rmtree(payload)
    for p in payload_files(SOURCE):
        target = payload / p.relative_to(SOURCE)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, target)
    command = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onefile',
               '--name', 'OctoFox-Server-Setup', '--distpath', str(HERE / 'dist'),
               '--workpath', str(build / 'freeze'), '--specpath', str(build),
               '--paths', str(SOURCE / 'tools'), '--add-data', str(payload) + ':library',
               '--add-data', str(HERE / 'web') + ':web']
    if sys.platform == 'win32':
        command += ['--windowed']
    elif sys.platform == 'darwin':
        command += ['--windowed', '--osx-bundle-identifier', 'art.abysstail.octofox.server-setup']
    command += [str(HERE / 'installer.py')]
    subprocess.run(command, check=True, cwd=HERE)
    if sys.platform.startswith('linux'):
        binary = HERE / 'dist/OctoFox-Server-Setup'
        binary.chmod(0o755)
    print('Built server installer in ' + str(HERE / 'dist'))


if __name__ == '__main__':
    main()
