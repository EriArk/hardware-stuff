"""Versioned, bounded archives. Never extract paths supplied by a ZIP file."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import zipfile

FORMAT = 'octofox-backup'
VERSION = 1
MAX_FILES = 200_000
MAX_ARCHIVE_BYTES = 20 * 1024**3
MAX_BYTES = 200 * 1024**3
ROOTS = {'library', 'booklore', 'catalog'}
EXCLUDED = {'instance-id', 'companion-setup-key', 'speech-cache'}


class BackupError(Exception):
    pass


def digest(path):
    with open(path, 'rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def safe_name(name):
    parts = PurePosixPath(name).parts
    return (bool(parts) and not name.startswith('/') and '\\' not in name and ':' not in name
            and all(p not in ('', '.', '..') for p in name.split('/'))
            and (name == 'booklore.sql' or len(parts) > 1 and parts[0] in ROOTS)
            and not (parts[0] == 'library' and parts[1] in EXCLUDED))


def sync_directory(path):
    if os.name != 'nt':
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def create(path, roots, sql, *, created, images):
    files = {'booklore.sql': Path(sql)}
    for label, root in roots.items():
        for item in sorted(root.rglob('*')):
            relative = item.relative_to(root)
            if label == 'library' and relative.parts[0] in EXCLUDED:
                continue
            if item.is_symlink() or (not item.is_dir() and not item.is_file()):
                raise BackupError('Unsupported file in server data; backup was not created.')
            if item.is_file():
                files[label + '/' + relative.as_posix()] = item
    if len(files) > MAX_FILES or sum(p.stat().st_size for p in files.values()) > MAX_BYTES:
        raise BackupError('This library exceeds the backup size limit.')
    manifest = {'format': FORMAT, 'version': VERSION, 'created': created, 'images': images, 'files': {}}
    partial = path.with_suffix('.partial')
    try:
        with zipfile.ZipFile(partial, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
            for name, item in files.items():
                manifest['files'][name] = {'bytes': item.stat().st_size, 'sha256': digest(item)}
                archive.write(item, name)
            archive.writestr('manifest.json', json.dumps(manifest, ensure_ascii=False))
        if partial.stat().st_size > MAX_ARCHIVE_BYTES:
            raise BackupError('The compressed backup exceeds the 20 GB archive limit.')
        with partial.open('r+b') as saved:
            os.fsync(saved.fileno())
        os.replace(partial, path)
        sync_directory(path.parent)
    finally:
        partial.unlink(missing_ok=True)
    return manifest


def inspect(path, destination=None):
    """Validate *all* names, lengths and checksums before modifying server data."""
    try:
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            names = [i.filename for i in infos]
            if len(infos) > MAX_FILES + 1 or len(names) != len(set(names)):
                raise BackupError('Duplicate entries or too many files in backup.')
            info = archive.getinfo('manifest.json')
            if info.file_size > 48 * 1024**2:
                raise BackupError('Backup manifest is too large.')
            manifest = json.loads(archive.read(info))
            if manifest.get('format') != FORMAT or manifest.get('version') != VERSION:
                raise BackupError('Unsupported backup format or version.')
            files = manifest['files']
            if not isinstance(files, dict) or set(names) != set(files) | {'manifest.json'}:
                raise BackupError('Backup file list does not match its manifest.')
            if not {'booklore.sql', 'library/library.sqlite3'} <= set(files):
                raise BackupError('Backup is missing an account or library database.')
            if sum(i.file_size for i in infos) > MAX_BYTES:
                raise BackupError('Backup expands beyond the size limit.')
            for item in infos:
                if item.filename == 'manifest.json':
                    continue
                mode = item.external_attr >> 16
                if (not safe_name(item.filename) or item.is_dir() or item.flag_bits & 1
                        or stat.S_IFMT(mode) not in (0, stat.S_IFREG)):
                    raise BackupError('Backup contains an unsafe file entry.')
                expected = files[item.filename]
                if item.file_size != expected['bytes']:
                    raise BackupError('Backup file size does not match.')
                hasher = hashlib.sha256()
                target = None
                if destination is not None:
                    target = destination / item.filename
                    target.parent.mkdir(parents=True, exist_ok=True)
                output = open(target, 'xb') if target else None
                try:
                    with archive.open(item) as source:
                        while chunk := source.read(1024 * 1024):
                            hasher.update(chunk)
                            if output:
                                output.write(chunk)
                finally:
                    if output:
                        output.close()
                if hasher.hexdigest() != expected['sha256']:
                    raise BackupError('Backup checksum failed. No server data was changed.')
            return manifest
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile, RuntimeError):
        raise BackupError('The backup is damaged or is not an OctoFox backup.') from None
