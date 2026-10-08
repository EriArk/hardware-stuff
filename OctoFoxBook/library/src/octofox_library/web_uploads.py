"""Account-private FB2/EPUB files; never publish uploads into the shared OPDS index."""
import base64
import hashlib
import json
import os
import re
import secrets
import time
import unicodedata
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from octofox_library.book_formats import parse_xml, normalize_fb2, MAX_DOCUMENT_BYTES
from octofox_library.web_errors import WebError
from octofox_library.epub import is_epub, reading_copy

MAX_UPLOAD = 16 * 1024 * 1024
MAX_ACCOUNT_UPLOAD_BYTES = 3 * 1024 * 1024 * 1024
MAX_ACCOUNT_UPLOAD_BOOKS = 1500


def parse_upload(payload, *, max_bytes=None):
    max_bytes = MAX_UPLOAD if max_bytes is None else max_bytes
    if not 256 <= len(payload) <= max_bytes:
        raise ValueError(f"Выбери FB2 размером от 256 байт до {max_bytes // (1024 * 1024)} МБ")
    if re.search(rb"<!\s*(DOCTYPE|ENTITY)", payload.replace(b"\x00", b""), re.I):
        raise ValueError("FB2 с DTD или внешними сущностями не поддерживается")
    try:
        root = parse_xml(payload, max_bytes=max_bytes)
    except ET.ParseError as error:
        raise ValueError("Не удалось прочитать FB2") from error
    if root.tag.rsplit("}", 1)[-1] != "FictionBook":
        raise ValueError("Ожидалась книга в формате FB2, не архив")
    info = root.find("./{*}description/{*}title-info")
    body = root.find("{*}body")
    if info is None or body is None or not (any(t.strip() for t in body.itertext()) or body.find('.//{*}image') is not None):
        raise ValueError("В FB2 не найдено описание или текст книги")
    title = info.findtext("{*}book-title", "").strip()[:500]
    if not title:
        raise ValueError("В FB2 не указано название книги")
    authors = []
    for author in info.findall("{*}author")[:30]:
        name = " ".join(filter(None, (author.findtext("{*}" + part, "").strip() for part in ("first-name", "middle-name", "last-name"))))
        authors.append((name or author.findtext("{*}nickname", "").strip())[:500])
    annotation = info.find("{*}annotation")
    sequence = info.find("{*}sequence")
    record = {"title": title, "authors": authors, "author": ", ".join(authors),
              "summary": " ".join(annotation.itertext())[:16000] if annotation is not None else "",
              "language": info.findtext("{*}lang", ""),
              "genres": [n.text[:200] for n in info.findall("{*}genre")[:100] if n.text],
              "series": sequence.get("name", "")[:500] if sequence is not None else "",
              "seriesNumber": sequence.get("number", "")[:30] if sequence is not None else "",
              "updated": datetime.now(timezone.utc).isoformat(), "privateUpload": True}
    cover = b""
    image = info.find("./{*}coverpage/{*}image")
    href = next((v[1:] for k, v in image.attrib.items() if k.rsplit("}", 1)[-1] == "href" and v.startswith("#")), "") if image is not None else ""
    if href:
        for binary in root.findall("{*}binary"):
            if binary.get("id") == href and binary.text and len(binary.text) < 6 * 1024 * 1024:
                try:
                    candidate = base64.b64decode(re.sub(r"\s+", "", binary.text), validate=True)
                    if len(candidate) <= 4 * 1024 * 1024 and image_type(candidate):
                        cover = candidate
                except ValueError:
                    pass
                break
    return record, cover


def image_type(data):
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "image/webp"
    return ""


class UploadsMixin:
    @staticmethod
    def uploaded_collection(db, owner):
        from octofox_library.web_catalog import normalized
        key = normalized('Загруженные')
        row = db.execute('SELECT id FROM personal_collections WHERE owner=? AND name_key=?', (owner, key)).fetchone()
        if row:
            return row[0]
        identity = "uploads-" + secrets.token_hex(12)
        db.execute('INSERT INTO personal_collections VALUES (?,?,?,?,?)',
                   (owner, identity, 'Загруженные', key, time.time()))
        return identity

    def _private_path(self, owner, book):
        directory = self.database.parent / "uploads" / hashlib.sha256(owner.encode()).hexdigest()
        return directory / (book + ".fb2")

    def upload_path(self, owner, book):
        with self.db() as db:
            row = db.execute("SELECT 1 FROM uploaded_books WHERE owner=? AND book=?", (owner, book)).fetchone()
        return self._private_path(owner, book) if row else None

    def upload_cover(self, owner, book):
        path = self.upload_path(owner, book)
        if not path or not path.with_suffix(".cover").is_file():
            raise WebError(404, "Нет обложки")
        data = path.with_suffix(".cover").read_bytes()
        return data, image_type(data)

    def upload_book(self, owner, payload, *, max_bytes=None, filename='', modified=0):
        if not isinstance(filename, str) or len(filename) > 255 or any(ord(c) < 32 or c in '/\\' for c in filename):
            raise WebError(400, 'Некорректное имя файла')
        if type(modified) is not int or not 0 <= modified <= 4102444800000:
            raise WebError(400, 'Некорректная дата файла')
        name_key = unicodedata.normalize('NFC', filename).casefold()
        source = payload
        source_epub = is_epub(source)
        try:
            if not 256 <= len(source) <= (max_bytes or MAX_UPLOAD):
                raise ValueError('Выбери FB2 или EPUB размером от 256 байт до 16 МБ')
            payload, repairs = (reading_copy(source), ()) if source_epub else normalize_fb2(source, max_bytes=max_bytes or MAX_UPLOAD)
            record, cover = parse_upload(payload, max_bytes=MAX_DOCUMENT_BYTES if source_epub or repairs else max_bytes)
            if source_epub:
                record['format'] = 'epub'
        except ValueError as error:
            raise WebError(422, str(error)) from error
        digest = hashlib.sha256(source).hexdigest()
        stored_size = len(payload) + (len(source) if source_epub or repairs else 0)
        backups, partials = {}, []
        committed = False
        outcome = 'duplicate'
        # Serialize against chapter cache fills: no stale old text after replacing.
        with self.read_gate:
            try:
                with self.db() as db:
                    db.execute('BEGIN IMMEDIATE')
                    existing = db.execute('SELECT book FROM uploaded_books WHERE owner=? AND digest=?', (owner, digest)).fetchone()
                    named = db.execute('SELECT s.*,u.bytes FROM upload_sources s JOIN uploaded_books u ON u.book=s.book AND u.owner=s.owner WHERE s.owner=? AND s.name_key=?', (owner, name_key)).fetchone() if name_key else None
                    replace = not existing and named and (modified > named['modified'] or len(source) > named['size'])
                    write = not existing and (not named or replace)
                    if existing:
                        book = existing[0]
                    elif named:
                        book = named['book']
                        outcome = 'replaced' if replace else 'kept'
                    else:
                        outcome = 'added'
                        book = str(900000000000 + secrets.randbelow(99999999999))
                        while db.execute('SELECT 1 FROM books WHERE id=? UNION SELECT 1 FROM uploaded_books WHERE book=?', (book, book)).fetchone():
                            book = str(900000000000 + secrets.randbelow(99999999999))
                    if write:
                        count, size = db.execute('SELECT count(*),coalesce(sum(bytes),0) FROM uploaded_books WHERE owner=?', (owner,)).fetchone()
                        if (not replace and count >= MAX_ACCOUNT_UPLOAD_BOOKS) or size - (named['bytes'] if replace else 0) + stored_size > MAX_ACCOUNT_UPLOAD_BYTES:
                            raise WebError(413, 'Личная загрузка ограничена 1500 файлами и 3 ГБ на аккаунт')
                        path = self._private_path(owner, book)
                        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                        artifacts = {path: payload, path.with_suffix('.cover'): cover,
                                     path.with_suffix('.epub'): source if source_epub else b'',
                                     path.with_suffix('.original.source'): source if repairs else b''}
                        token = secrets.token_hex(12)
                        for artifact, data in artifacts.items():
                            backup = artifact.with_name(artifact.name + '.' + token + '.backup')
                            # Hardlink old inode before replacing: concurrent readers never
                            # see a missing file; failure restores both files and DB.
                            if artifact.exists():
                                os.link(artifact, backup)
                                backups[artifact] = backup
                            else:
                                backups[artifact] = None
                            if data:
                                partial = artifact.with_name(artifact.name + '.' + token + '.part')
                                partials.append(partial)
                                with partial.open('xb') as file:
                                    file.write(data); file.flush(); os.fsync(file.fileno())
                                partial.chmod(0o600)
                                os.replace(partial, artifact)
                            else:
                                artifact.unlink(missing_ok=True)
                        record.update(id=book, cover=f'/reader-api/books/{book}/cover')
                        if replace:
                            db.execute('UPDATE uploaded_books SET digest=?,bytes=? WHERE owner=? AND book=?', (digest, stored_size, owner, book))
                            db.execute('UPDATE upload_sources SET modified=?,size=? WHERE owner=? AND book=?', (modified, len(source), owner, book))
                        else:
                            db.execute('INSERT INTO uploaded_books VALUES (?,?,?,?,?)', (owner, book, digest, stored_size, time.time()))
                        db.execute('INSERT OR REPLACE INTO books VALUES (?,?,?,?,?,?,?,?)', (owner, book, record['title'].casefold(), '', record['author'], record['series'], record['updated'], json.dumps(record, ensure_ascii=False)))
                        self._write_terms(db, owner, book, record)
                    if name_key and outcome != 'kept' and (not named or named['book'] == book):
                        db.execute('INSERT INTO upload_sources VALUES (?,?,?,?,?) ON CONFLICT(owner,name_key) DO UPDATE SET modified=max(modified,excluded.modified),size=excluded.size', (owner, name_key, book, modified, len(source)))
                    db.execute('INSERT OR IGNORE INTO personal_library VALUES (?,?,?)', (owner, book, time.time()))
                    collection = self.uploaded_collection(db, owner)
                    db.execute('INSERT OR IGNORE INTO personal_collection_books VALUES (?,?,?)', (owner, collection, book))
                committed = True
                if write:
                    with self.lock:
                        self.read_cache.pop((owner, book), None)
                    with self.speech.lock:
                        for cache in (self.speech.plans, self.speech.wishes):
                            for key in list(cache):
                                if key[:2] == (owner, book):
                                    cache.pop(key, None)
                        for identity, stream in list(self.speech.streams.items()):
                            if stream['owner'] == owner and stream['book'] == book:
                                self.speech.streams.pop(identity, None)
            except Exception:
                if not committed:
                    for artifact, backup in backups.items():
                        if backup:
                            os.replace(backup, artifact)
                        else:
                            artifact.unlink(missing_ok=True)
                raise
            finally:
                for artifact in partials + [b for b in backups.values() if b]:
                    artifact.unlink(missing_ok=True)
        result = self.book(owner, book)
        result['uploadResult'] = outcome
        return result
