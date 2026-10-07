"""Personal mobile library and durable, account-scoped reader delivery queue.

BookLore OPDS remains the source of books and credentials. Only catalogue
metadata, reading positions and delivery state are stored here, never passwords.
"""

from __future__ import annotations

import base64
import hashlib
import html
import json
import logging
import os
import re
import secrets
import sqlite3
import threading
import time
import xml.etree.ElementTree as ET
from collections import OrderedDict
from contextlib import contextmanager
from dataclasses import dataclass
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlsplit
from urllib.request import HTTPRedirectHandler, build_opener

from octofox_library.book_formats import MAX_DOCUMENT_BYTES
from octofox_library.opds_facade import (
    ATOM,
    CALIBRE,
    DC,
    PREFIX,
    FacadeSettings,
    UpstreamClient,
    UpstreamFailure,
)
from octofox_library.web_catalog import (
    GENRE_ALIASES,
    GENRE_CATEGORIES,
    TAG_CATEGORIES,
    book_terms,
    decorate_record,
    normalized,
)
from octofox_library.web_collections import COLLECTIONS, collection_filter
from octofox_library.web_uploads import UploadsMixin
from octofox_library.web_personal_collections import PersonalCollectionsMixin
from octofox_library.web_errors import WebError
from octofox_library.web_speech import BookSpeech, SpeechError
from octofox_library.companion import Companion
from octofox_library.network import NetworkSettings

LOGGER = logging.getLogger(__name__)
STATIC = Path(__file__).with_name("web")
MAX_BOOK = MAX_DOCUMENT_BYTES
SESSION_SECONDS = 7 * 86400
DEVICE_RE = re.compile(r"[A-Za-z0-9_-]{8,64}\Z")
BOOK_RE = re.compile(r"[0-9]{1,12}\Z")


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def plain(value: str) -> str:
    # OPDS descriptions sometimes contain escaped XHTML. They are text in UI.
    return html.unescape(re.sub(r"<[^>]*>", "", value)).strip()


def entry_record(entry: ET.Element) -> dict | None:
    links = entry.findall(f"{{{ATOM}}}link")
    acquisition = next(
        (link.get("href", "") for link in links if "acquisition" in link.get("rel", "")), ""
    )
    match = re.fullmatch(r"/api/v1/opds/([0-9]{1,12})/download", urlsplit(acquisition).path)
    if not match:
        return None

    def value(name, namespace=ATOM):
        return entry.findtext(f"{{{namespace}}}{name}", "").strip()

    authors = [
        node.findtext(f"{{{ATOM}}}name", "").strip() for node in entry.findall(f"{{{ATOM}}}author")
    ]
    # BookLore uses Atom meta belongs-to-collection/group-position, while some
    # other OPDS feeds use the Calibre namespace. Support both without guessing.
    metas = entry.findall(f"{{{ATOM}}}meta")
    collection = next((m for m in metas if m.get("property") == "belongs-to-collection"), None)
    series = value("series", CALIBRE)
    number = value("series_index", CALIBRE)
    if collection is not None:
        series = (collection.text or "").strip() or series
        refines = "#" + collection.get("id", "series")
        number = next(
            (
                (m.text or "").strip()
                for m in metas
                if m.get("property") == "group-position" and m.get("refines") == refines
            ),
            number,
        )
    return {
        "id": match[1],
        "format": "epub" if any(link.get('type') == 'application/epub+zip' and link.get('href') == acquisition for link in links) else "fb2",
        "title": value("title"),
        "authors": authors,
        "author": ", ".join(authors),
        "series": series,
        "seriesNumber": number,
        "summary": plain(value("summary") or value("content"))[:16000],
        "language": value("language", DC),
        "updated": value("updated"),
        "genres": [
            c.get("term", "") for c in entry.findall(f"{{{ATOM}}}category") if c.get("term")
        ],
        "cover": f"/reader-api/books/{match[1]}/cover",
    }


def fb2_chapters(payload: bytes) -> list[dict]:
    from octofox_library.epub import reading_copy
    try:
        payload = reading_copy(payload)
    except ValueError as error:
        raise WebError(422, str(error)) from error
    if len(payload) > MAX_BOOK or re.search(
        rb"<!\s*(DOCTYPE|ENTITY)", payload.replace(b"\x00", b""), re.I
    ):
        raise WebError(422, "Эта книга не поддерживается встроенной читалкой")
    try:
        root = ET.fromstring(payload)
    except ET.ParseError as error:
        raise WebError(422, "Не удалось прочитать FB2") from error
    if root.tag.rsplit("}", 1)[-1] != "FictionBook":
        raise WebError(422, "Ожидалась книга в формате FB2")
    tags = {
        "p": "p",
        "v": "p",
        "subtitle": "h3",
        "title": "h2",
        "emphasis": "em",
        "strong": "strong",
        "strikethrough": "s",
        "sub": "sub",
        "sup": "sup",
        "cite": "blockquote",
        "epigraph": "blockquote",
    }

    def render(node):
        name = node.tag.rsplit("}", 1)[-1]
        if name in {"binary", "description", "image"}:
            return ""
        if name == "empty-line":
            return "<br>"
        inside = html.escape(node.text or "") + "".join(
            render(child) + html.escape(child.tail or "") for child in node
        )
        tag = tags.get(name)
        return f"<{tag}>{inside}</{tag}>" if tag else inside

    chapters = []
    for body in root:
        if body.tag.rsplit("}", 1)[-1] != "body":
            continue
        for section in body:
            fragment = render(section)
            if not fragment.strip():
                continue
            title_node = next((n for n in section if n.tag.rsplit("}", 1)[-1] == "title"), None)
            title = " ".join(title_node.itertext()).strip() if title_node is not None else ""
            # Split only at block boundaries, preserving valid, sanitized HTML.
            chunks = re.split(r"(?<=</p>)", fragment)
            current = ""
            for chunk in chunks:
                current += chunk
                if len(current) > 45000:
                    chapters.append(
                        {"title": title or f"Часть {len(chapters) + 1}", "html": current}
                    )
                    current = ""
            if current:
                chapters.append({"title": title or f"Часть {len(chapters) + 1}", "html": current})
    if not chapters:
        raise WebError(422, "В книге не найден текст")
    return chapters


@dataclass
class Session:
    owner: str
    authorization: str
    csrf: str
    expires: float


class LibraryWeb(UploadsMixin, PersonalCollectionsMixin):
    def __init__(self, database: Path, upstream: str, origin: str):
        self.database, self.origin = database, origin.rstrip("/")
        database.parent.mkdir(parents=True, exist_ok=True)
        self.client = UpstreamClient(FacadeSettings(upstream_url=upstream, public_origin=origin))
        self.sessions: OrderedDict[str, Session] = OrderedDict()
        self.lock = threading.RLock()
        self.indexing: set[str] = set()
        self.index_status: dict[str, str] = {}
        self.index_retry: dict[str, float] = {}
        # All accounts share the same upstream and SQLite writer. Do not let
        # simultaneous logins launch competing full-catalog refreshes.
        self.index_gate = threading.BoundedSemaphore(1)
        self.auth_cache: OrderedDict[str, tuple[str, float]] = OrderedDict()
        self.attempts: dict[str, list[float]] = {}
        self.read_cache: OrderedDict[tuple[str, str], list[dict]] = OrderedDict()
        self.read_gate = threading.Lock()
        self.download_gate = threading.BoundedSemaphore(2)
        with self.db() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS books (
                    owner TEXT NOT NULL, id TEXT NOT NULL, title TEXT NOT NULL,
                    search TEXT NOT NULL, author TEXT NOT NULL, series TEXT NOT NULL,
                    updated TEXT NOT NULL, record TEXT NOT NULL, PRIMARY KEY(owner,id));
                CREATE TABLE IF NOT EXISTS catalog_state (
                    owner TEXT PRIMARY KEY, refreshed REAL NOT NULL);
                CREATE TABLE IF NOT EXISTS devices (
                    owner TEXT NOT NULL, id TEXT NOT NULL, name TEXT NOT NULL,
                    last_seen REAL NOT NULL, PRIMARY KEY(owner,id));
                CREATE TABLE IF NOT EXISTS delivery (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, owner TEXT NOT NULL,
                    device TEXT NOT NULL, book TEXT NOT NULL, title TEXT NOT NULL,
                    state TEXT NOT NULL DEFAULT 'queued', created REAL NOT NULL,
                    updated REAL NOT NULL, error TEXT NOT NULL DEFAULT '',
                    sha256 TEXT NOT NULL DEFAULT '', UNIQUE(owner,device,book));
                CREATE INDEX IF NOT EXISTS delivery_poll ON delivery(owner,device,state,id);
                CREATE TABLE IF NOT EXISTS reading (
                    owner TEXT NOT NULL, book TEXT NOT NULL, chapter INTEGER NOT NULL DEFAULT 0,
                    offset REAL NOT NULL DEFAULT 0, shelf TEXT NOT NULL DEFAULT '',
                    updated REAL NOT NULL, PRIMARY KEY(owner,book));
                CREATE TABLE IF NOT EXISTS personal_library (
                    owner TEXT NOT NULL, book TEXT NOT NULL, added REAL NOT NULL,
                    PRIMARY KEY(owner,book));
                CREATE TABLE IF NOT EXISTS favorite_books (
                    owner TEXT NOT NULL, book TEXT NOT NULL, added REAL NOT NULL,
                    PRIMARY KEY(owner,book));
                CREATE TABLE IF NOT EXISTS personal_collections (
                    owner TEXT NOT NULL, id TEXT NOT NULL, name TEXT NOT NULL,
                    name_key TEXT NOT NULL, created REAL NOT NULL,
                    PRIMARY KEY(owner,id), UNIQUE(owner,name_key));
                CREATE TABLE IF NOT EXISTS personal_collection_books (
                    owner TEXT NOT NULL, collection TEXT NOT NULL, book TEXT NOT NULL,
                    PRIMARY KEY(owner,collection,book));
                CREATE INDEX IF NOT EXISTS personal_collection_book_lookup
                    ON personal_collection_books(owner,book,collection);
                CREATE TABLE IF NOT EXISTS reading_activity (
                    owner TEXT NOT NULL, book TEXT NOT NULL, last_read REAL NOT NULL,
                    PRIMARY KEY(owner,book));
                CREATE TABLE IF NOT EXISTS reader_bookmarks (
                    owner TEXT NOT NULL, book TEXT NOT NULL, id TEXT NOT NULL,
                    chapter INTEGER NOT NULL, block INTEGER NOT NULL, char INTEGER NOT NULL,
                    label TEXT NOT NULL, excerpt TEXT NOT NULL, created REAL NOT NULL,
                    PRIMARY KEY(owner,book,id), UNIQUE(owner,book,chapter,block,char));
                CREATE TABLE IF NOT EXISTS book_facets (
                    owner TEXT NOT NULL, book TEXT NOT NULL, kind TEXT NOT NULL,
                    value TEXT NOT NULL, label TEXT NOT NULL, search TEXT NOT NULL,
                    group_id TEXT NOT NULL, PRIMARY KEY(owner,book,kind,value));
                CREATE INDEX IF NOT EXISTS facets_lookup ON book_facets(owner,kind,value,book);
                CREATE INDEX IF NOT EXISTS facets_browse ON book_facets(owner,kind,group_id,search);
                CREATE INDEX IF NOT EXISTS books_recent ON books(owner,CAST(id AS INTEGER) DESC,id DESC);
                CREATE INDEX IF NOT EXISTS books_title ON books(owner,normalized(title),CAST(id AS INTEGER));
                CREATE INDEX IF NOT EXISTS books_title_desc ON books(owner,normalized(title) DESC,CAST(id AS INTEGER));
                CREATE INDEX IF NOT EXISTS books_author ON books(owner,normalized(author),normalized(title),id);
                CREATE INDEX IF NOT EXISTS books_series ON books(owner,normalized(series),
                    CAST(json_extract(record,'$.seriesNumber') AS REAL),title,id);
                CREATE TABLE IF NOT EXISTS web_schema (version INTEGER PRIMARY KEY);
            """)
            if not db.execute("SELECT 1 FROM web_schema WHERE version=1").fetchone():
                # Additive migration. Delivery, credentials and reading positions
                # are not touched; existing OPDS metadata needs no re-download.
                for row in db.execute("SELECT owner,id,record FROM books"):
                    self._write_terms(db, row[0], row[1], json.loads(row[2]))
                db.execute("INSERT INTO web_schema VALUES (1)")
                db.execute("PRAGMA optimize")
            if not db.execute("SELECT 1 FROM web_schema WHERE version=2").fetchone():
                # Re-read catalogue metadata once to recover previously ignored
                # BookLore series; do not download books or remove cached entries.
                db.execute("DELETE FROM catalog_state")
                db.execute("INSERT INTO web_schema VALUES (2)")
            if not db.execute("SELECT 1 FROM web_schema WHERE version=3").fetchone():
                # Preserve old favorites independently of the reading-status shelf.
                db.execute(
                    "INSERT OR IGNORE INTO favorite_books SELECT owner,book,updated "
                    "FROM reading WHERE shelf='favorite'"
                )
                db.execute(
                    "INSERT OR IGNORE INTO reading_activity SELECT owner,book,updated "
                    "FROM reading WHERE chapter>0 OR offset>0 OR shelf IN ('reading','read')"
                )
                db.execute("INSERT INTO web_schema VALUES (3)")
            if not db.execute("SELECT 1 FROM web_schema WHERE version=4").fetchone():
                if "anchor" not in {r[1] for r in db.execute("PRAGMA table_info(reading)")}:
                    db.execute("ALTER TABLE reading ADD COLUMN anchor TEXT")
                db.execute("INSERT INTO web_schema VALUES (4)")
            if not db.execute("SELECT 1 FROM web_schema WHERE version=5").fetchone():
                columns = {r[1] for r in db.execute("PRAGMA table_info(delivery)")}
                if "action" not in columns:
                    db.execute("ALTER TABLE delivery ADD COLUMN action TEXT NOT NULL DEFAULT 'download'")
                if "present" not in columns:
                    db.execute("ALTER TABLE delivery ADD COLUMN present INTEGER NOT NULL DEFAULT 0")
                db.execute("UPDATE delivery SET present=1 WHERE state='delivered'")
                db.execute("INSERT INTO web_schema VALUES (5)")
            db.execute("""CREATE TABLE IF NOT EXISTS uploaded_books (
                owner TEXT NOT NULL, book TEXT NOT NULL PRIMARY KEY, digest TEXT NOT NULL,
                bytes INTEGER NOT NULL, created REAL NOT NULL, UNIQUE(owner,digest))""")
            db.execute('''CREATE TABLE IF NOT EXISTS upload_sources (
                owner TEXT NOT NULL, name_key TEXT NOT NULL, book TEXT NOT NULL,
                modified INTEGER NOT NULL, size INTEGER NOT NULL, PRIMARY KEY(owner,name_key))''')
            if not db.execute('SELECT 1 FROM web_schema WHERE version=6').fetchone():
                for row in db.execute('SELECT DISTINCT u.owner FROM uploaded_books u JOIN personal_library p ON p.owner=u.owner AND p.book=u.book').fetchall():
                    owner = row[0]
                    collection = self.uploaded_collection(db, owner)
                    db.execute('INSERT OR IGNORE INTO personal_collection_books SELECT u.owner,?,u.book FROM uploaded_books u JOIN personal_library p ON p.owner=u.owner AND p.book=u.book WHERE u.owner=?', (collection, owner))
                db.execute('INSERT INTO web_schema VALUES (6)')
            db.execute('PRAGMA optimize')
        self.upload_gate = threading.BoundedSemaphore(1)
        self.speech = BookSpeech(self, os.environ.get("OCTOFOX_SPEECH_SOCKET", ""))
        self.network = NetworkSettings(self)
        self.companion = Companion(self, upstream)

    @contextmanager
    def db(self):
        db = sqlite3.connect(self.database, timeout=10)
        db.row_factory = sqlite3.Row
        db.create_function("normalized", 1, normalized, deterministic=True)
        try:
            yield db
            db.commit()
        finally:
            db.close()

    def authenticate(self, authorization: str) -> str:
        try:
            if not authorization.startswith("Basic ") or len(authorization) > 1024:
                raise ValueError()
            decoded = base64.b64decode(authorization[6:], validate=True).decode("utf-8")
            owner, password = decoded.split(":", 1)
            if not owner or not password or len(owner) > 64 or any(ord(c) < 32 for c in owner):
                raise ValueError()
        except (ValueError, UnicodeError) as error:
            raise WebError(401, "Неверный логин или пароль библиотеки") from error
        key = hashlib.sha256(authorization.encode()).hexdigest()
        with self.lock:
            cached = self.auth_cache.get(key)
            if cached and cached[1] > time.time():
                return cached[0]
        self.client.fetch_xml("", (), authorization)
        with self.lock:
            self.auth_cache[key] = (owner, time.time() + 60)
            while len(self.auth_cache) > 64:
                self.auth_cache.popitem(last=False)
        return owner

    def throttle(self, address: str, limit: int = 12):
        with self.lock:
            now = time.time()
            attempts = [t for t in self.attempts.get(address, []) if now - t < 60]
            if len(attempts) >= limit:
                raise WebError(429, "Подождите минуту перед следующей попыткой входа")
            attempts.append(now)
            self.attempts[address] = attempts
            if len(self.attempts) > 2048:
                self.attempts = {k: v for k, v in self.attempts.items() if now - v[-1] < 60}

    def login(self, username: str, password: str) -> tuple[str, Session]:
        if ":" in username or len(password) > 512:
            raise WebError(401, "Неверный логин или пароль библиотеки")
        authorization = "Basic " + base64.b64encode(f"{username}:{password}".encode()).decode()
        owner = self.authenticate(authorization)
        session = Session(
            owner, authorization, secrets.token_urlsafe(24), time.time() + SESSION_SECONDS
        )
        token = secrets.token_urlsafe(32)
        with self.lock:
            self.sessions[token] = session
            while len(self.sessions) > 128:
                self.sessions.popitem(last=False)
        self.ensure_index(session)
        self.speech.warm_library(session)
        return token, session

    def ensure_index(self, session: Session):
        with self.lock, self.db() as db:
            row = db.execute(
                "SELECT refreshed FROM catalog_state WHERE owner=?", (session.owner,)
            ).fetchone()
            if (
                session.owner in self.indexing
                or (row and time.time() - row[0] < 900)
                or self.index_retry.get(session.owner, 0) > time.time()
            ):
                return
            if not self.index_gate.acquire(blocking=False):
                return  # Existing cached catalogue remains readable; polling retries later.
            self.indexing.add(session.owner)
            self.index_status[session.owner] = "loading"
        def refresh():
            try:
                self._index(session)
            finally:
                self.index_gate.release()
        try:
            threading.Thread(target=refresh, daemon=True).start()
        except Exception:
            self.indexing.discard(session.owner)
            self.index_gate.release()
            raise

    def _index(self, session: Session):
        try:
            batch = []
            seen = []
            for entry in self.client.iter_catalog_entries(session.authorization):
                record = entry_record(entry)
                if record:
                    seen.append(record["id"])
                    search = " ".join(
                        [
                            record["title"],
                            record["author"],
                            record["series"],
                            " ".join(record["genres"]),
                        ]
                    ).casefold()
                    batch.append(
                        (
                            session.owner,
                            record["id"],
                            record["title"].casefold(),
                            search,
                            record["author"],
                            record["series"],
                            record["updated"],
                            json.dumps(record, ensure_ascii=False),
                        )
                    )
                if len(batch) >= 100:
                    self._index_batch(batch)
                    batch.clear()
            self._index_batch(batch)
            with self.db() as db:
                # Only prune after a complete scan; retain the old catalogue on
                # upstream failure, including saved reading positions/queues.
                db.execute(
                    "DELETE FROM books WHERE owner=? AND id NOT IN "
                    "(SELECT value FROM json_each(?)) AND NOT EXISTS "
                    "(SELECT 1 FROM uploaded_books u WHERE u.owner=books.owner AND u.book=books.id)",
                    (session.owner, json.dumps(seen)),
                )
                db.execute(
                    "INSERT OR REPLACE INTO catalog_state VALUES (?,?)",
                    (session.owner, time.time()),
                )
                db.execute(
                    "DELETE FROM book_facets WHERE owner=? AND NOT EXISTS "
                    "(SELECT 1 FROM books b WHERE b.owner=book_facets.owner "
                    "AND b.id=book_facets.book)",
                    (session.owner,),
                )
            self.index_status[session.owner] = "ready"
        except Exception:
            LOGGER.exception("OPDS metadata refresh failed")
            self.index_status[session.owner] = "error"
            self.index_retry[session.owner] = time.time() + 60
        finally:
            with self.lock:
                self.indexing.discard(session.owner)

    def _index_batch(self, rows):
        if not rows:
            return
        with self.db() as db:
            # Most refresh entries are unchanged. Avoid rewriting every book and
            # deleting/rebuilding all its facets each time someone opens the site.
            changed = []
            for row in rows:
                old = db.execute("SELECT record FROM books WHERE owner=? AND id=?", row[:2]).fetchone()
                if old is None or old[0] != row[7]:
                    changed.append(row)
            db.executemany("INSERT OR REPLACE INTO books VALUES (?,?,?,?,?,?,?,?)", changed)
            for row in changed:
                self._write_terms(db, row[0], row[1], json.loads(row[7]))

    @staticmethod
    def _write_terms(db, owner, book_id, record):
        terms = book_terms(record)
        db.execute("DELETE FROM book_facets WHERE owner=? AND book=?", (owner, book_id))
        db.executemany(
            "INSERT INTO book_facets VALUES (?,?,?,?,?,?,?)",
            [(owner, book_id, *term) for term in terms],
        )
        search = normalized(
            " ".join(
                [
                    record["title"],
                    record.get("author", ""),
                    record.get("series", ""),
                    *(term[2] for term in terms),
                ]
            )
        )
        db.execute("UPDATE books SET search=? WHERE owner=? AND id=?", (search, owner, book_id))

    def book(self, owner, book_id):
        if not BOOK_RE.fullmatch(book_id):
            raise WebError(404, "Книга не найдена")
        with self.db() as db:
            row = db.execute(
                "SELECT record FROM books WHERE owner=? AND id=?", (owner, book_id)
            ).fetchone()
            state = db.execute(
                "SELECT chapter,offset,shelf,anchor FROM reading WHERE owner=? AND book=?",
                (owner, book_id),
            ).fetchone()
            saved = db.execute(
                "SELECT 1 FROM personal_library WHERE owner=? AND book=?", (owner, book_id)
            ).fetchone()
            favorite = db.execute(
                "SELECT 1 FROM favorite_books WHERE owner=? AND book=?", (owner, book_id)
            ).fetchone()
        if not row:
            raise WebError(404, "Книга ещё не появилась в каталоге")
        result = decorate_record(json.loads(row[0]))
        result["reading"] = dict(state) if state else {"chapter": 0, "offset": 0, "shelf": ""}
        anchor = result["reading"].pop("anchor", None)
        if anchor:
            result["reading"]["anchor"] = json.loads(anchor)
        result["inLibrary"] = bool(saved)
        result["isFavorite"] = bool(favorite)
        return result

    def favorite_book(self, owner, book_id, favorite):
        if type(favorite) is not bool:
            raise WebError(400, "Ожидалось состояние избранного")
        self.book(owner, book_id)
        with self.db() as db:
            if favorite:
                db.execute(
                    "INSERT OR IGNORE INTO favorite_books VALUES (?,?,?)",
                    (owner, book_id, time.time()),
                )
            else:
                db.execute("DELETE FROM favorite_books WHERE owner=? AND book=?", (owner, book_id))
        return {"isFavorite": favorite}

    def save_state(self, owner, book_id, data):
        self.book(owner, book_id)
        fields = {}
        if "chapter" in data:
            fields["chapter"] = max(0, min(100000, int(data["chapter"])))
        if "offset" in data:
            fields["offset"] = max(0, min(1, float(data["offset"])))
        if "anchor" in data:
            anchor = data["anchor"]
            if anchor is not None and (
                not isinstance(anchor, dict) or set(anchor) != {"block", "char"}
                or any(type(anchor[k]) is not int or not 0 <= anchor[k] <= 10000000 for k in anchor)
                or "chapter" not in data
            ):
                raise WebError(400, "Некорректная позиция текста")
            fields["anchor"] = json.dumps(anchor) if anchor is not None else None
        elif "chapter" in data or "offset" in data:
            fields["anchor"] = None  # Older open clients still save only scroll position.
        if "shelf" in data:
            if data["shelf"] not in {"", "want", "reading", "read", "favorite"}:
                raise WebError(400, "Неизвестная полка")
            if data["shelf"] == "favorite":
                self.favorite_book(owner, book_id, True)  # Old open tabs remain compatible.
            else:
                fields["shelf"] = data["shelf"]
        with self.db() as db:
            db.execute(
                "INSERT OR IGNORE INTO reading(owner,book,updated) VALUES (?,?,?)",
                (owner, book_id, time.time()),
            )
            if fields:
                # Update only supplied fields: setting a shelf must not overwrite
                # a concurrent position save with an earlier copy of that position.
                assignments = ",".join(f"{key}=?" for key in fields)
                db.execute(
                    f"UPDATE reading SET {assignments},updated=? WHERE owner=? AND book=?",
                    (*fields.values(), time.time(), owner, book_id),
                )
            if "chapter" in fields or "offset" in fields:
                db.execute(
                    "INSERT OR REPLACE INTO reading_activity VALUES (?,?,?)",
                    (owner, book_id, time.time()),
                )
        return {"ok": True}

    def bookmarks(self, owner, book_id, data=None):
        self.book(owner, book_id)
        with self.db() as db:
            if data is not None:
                action = data.get('action')
                if action not in {'add', 'rename', 'remove'}:
                    raise WebError(400, 'Неизвестное действие с закладкой')
                label = data.get('label', '')
                if action != 'remove' and (not isinstance(label, str) or not label.strip() or len(label) > 120):
                    raise WebError(400, 'Название закладки: от 1 до 120 символов')
                if action == 'add':
                    chapter, anchor, excerpt = data.get('chapter'), data.get('anchor'), data.get('excerpt', '')
                    if (type(chapter) is not int or not 0 <= chapter <= 100000
                            or not isinstance(anchor, dict) or set(anchor) != {'block', 'char'}
                            or any(type(anchor[k]) is not int or not 0 <= anchor[k] <= 10000000 for k in anchor)
                            or not isinstance(excerpt, str) or len(excerpt) > 240):
                        raise WebError(400, 'Некорректная позиция закладки')
                    # Serialize limit-check + insert; only this account/book is affected.
                    db.execute('BEGIN IMMEDIATE')
                    existing = db.execute('SELECT id FROM reader_bookmarks WHERE owner=? AND book=? AND chapter=? AND block=? AND char=?',
                                          (owner, book_id, chapter, anchor['block'], anchor['char'])).fetchone()
                    if not existing:
                        if db.execute('SELECT COUNT(*) FROM reader_bookmarks WHERE owner=? AND book=?', (owner, book_id)).fetchone()[0] >= 500:
                            raise WebError(400, 'В книге уже 500 закладок')
                        db.execute('INSERT INTO reader_bookmarks VALUES (?,?,?,?,?,?,?,?,?)',
                                   (owner, book_id, secrets.token_hex(12), chapter, anchor['block'], anchor['char'], label.strip(), excerpt, time.time()))
                else:
                    identity = data.get('id')
                    if not isinstance(identity, str) or not re.fullmatch(r'[a-f0-9]{24}', identity):
                        raise WebError(400, 'Некорректная закладка')
                    if action == 'remove':
                        result = db.execute('DELETE FROM reader_bookmarks WHERE owner=? AND book=? AND id=?', (owner, book_id, identity))
                    else:
                        result = db.execute('UPDATE reader_bookmarks SET label=? WHERE owner=? AND book=? AND id=?', (label.strip(), owner, book_id, identity))
                    if not result.rowcount:
                        raise WebError(404, 'Закладка не найдена')
            rows = db.execute('SELECT id,chapter,block,char,label,excerpt,created FROM reader_bookmarks WHERE owner=? AND book=? ORDER BY chapter,block,char',
                              (owner, book_id)).fetchall()
        return {'bookmarks': [dict(id=r['id'], chapter=r['chapter'], anchor=dict(block=r['block'], char=r['char']),
                                   label=r['label'], excerpt=r['excerpt'], created=r['created']) for r in rows]}

    def save_book(self, owner, book_id, saved):
        if type(saved) is not bool:
            raise WebError(400, "Ожидалось состояние сохранения книги")
        self.book(owner, book_id)
        with self.db() as db:
            if saved:
                db.execute(
                    "INSERT OR IGNORE INTO personal_library VALUES (?,?,?)",
                    (owner, book_id, time.time()),
                )
            else:
                # Remove the personal bookmark only, never the book, progress or queue.
                db.execute(
                    "DELETE FROM personal_library WHERE owner=? AND book=?", (owner, book_id)
                )
                db.execute('DELETE FROM personal_collection_books WHERE owner=? AND book=?', (owner, book_id))
        return {"inLibrary": saved}

    def download_book(self, session, book_id, *, original=False):
        from octofox_library.epub import is_epub, reading_copy
        book = self.book(session.owner, book_id)
        local = self.upload_path(session.owner, book_id)
        if local:
            if original and local.with_suffix('.epub').is_file():
                return local.with_suffix('.epub').read_bytes(), self.file_disposition(book, 'epub')
            return local.read_bytes(), self.file_disposition(book)
        with self.client.open(f"/{book_id}/download", (), session.authorization) as response:
            mime = response.headers.get_content_type()
            if mime not in {
                "application/x-fictionbook+xml",
                "application/fb2+xml",
                "application/xml",
                "text/xml",
                "application/octet-stream",
                "application/epub+zip",
            }:
                raise WebError(502, "Библиотека не вернула файл FB2 или EPUB")
            data = response.read(MAX_BOOK + 1)
        if not data or len(data) > MAX_BOOK:
            raise WebError(413, "Файл пуст или превышает допустимые 128 МБ")
        if original and is_epub(data):
            return data, self.file_disposition(book, 'epub')
        try:
            data = reading_copy(data)
        except ValueError as error:
            raise WebError(422, str(error)) from error
        return data, self.file_disposition(book)

    @staticmethod
    def file_disposition(book, extension='fb2'):
        book_id = book["id"]
        title = re.sub(r'[\x00-\x1f\x7f/\\:*?"<>|]', " ", book["title"]).strip(" .")[:120]
        filename = (title or f"book-{book_id}") + '.' + extension
        disposition = f'attachment; filename="book-{book_id}.{extension}"; '
        disposition += "filename*=UTF-8''" + quote(filename, safe="")
        return disposition

    def collections(self, session):
        self.ensure_index(session)
        items = []
        with self.db() as db:
            for key, definition in COLLECTIONS.items():
                predicate, params = collection_filter(key, session.owner)
                count = db.execute(
                    "SELECT count(*) FROM books b WHERE b.owner=? AND " + predicate
                    + " AND NOT EXISTS (SELECT 1 FROM uploaded_books u "
                    "WHERE u.owner=b.owner AND u.book=b.id)",
                    [session.owner, *params],
                ).fetchone()[0]
                items.append({k: definition[k] for k in ("id", "title", "description", "image")}
                             | {"count": count})
        return {"collections": items, **self.catalog_status(session, refresh=False)}

    def catalog(self, session, query):
        self.ensure_index(session)
        conditions, params = ["b.owner=?"], [session.owner]
        personal_collection = query.get('personalCollection', [''])[0]
        if personal_collection:
            with self.db() as db:
                self._collection(db, session.owner, personal_collection)
            conditions.append('EXISTS (SELECT 1 FROM personal_collection_books m WHERE m.owner=b.owner AND m.book=b.id AND m.collection=?)')
            params.append(personal_collection)
        collection = query.get("collection", [""])[0]
        if collection:
            if collection not in COLLECTIONS:
                raise WebError(400, "Подборка не найдена")
            predicate, values = collection_filter(collection, session.owner)
            conditions.append(predicate)
            params.extend(values)
        arrivals = query.get("new", [""])[0] == "1"
        if not personal_collection and query.get("personal", [""])[0] != "1" and not query.get("shelf", [""])[0]:
            conditions.append("NOT EXISTS (SELECT 1 FROM uploaded_books u WHERE u.owner=b.owner AND u.book=b.id)")
        if arrivals:
            # BookLore insertion IDs are stable across metadata edits. Select
            # the latest 100 BEFORE filtering and pagination, per account.
            conditions.append(
                "b.id IN (SELECT id FROM books WHERE owner=? AND NOT EXISTS "
                "(SELECT 1 FROM uploaded_books u WHERE u.owner=books.owner AND u.book=books.id) "
                "ORDER BY CAST(id AS INTEGER) DESC,id DESC LIMIT 100)"
            )
            params.append(session.owner)
        term = normalized(query.get("q", [""])[0][:200])
        scope = query.get("scope", ["all"])[0]
        for word in term.split():
            pattern = "%" + word.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%"
            if scope in {"author", "series", "genre", "tag"}:
                conditions.append(
                    "EXISTS (SELECT 1 FROM book_facets f WHERE f.owner=b.owner "
                    "AND f.book=b.id AND f.kind=? AND f.search LIKE ? ESCAPE '\\')"
                )
                params.extend((scope, pattern))
            else:
                field = "normalized(b.title)" if scope == "title" else "b.search"
                conditions.append(f"{field} LIKE ? ESCAPE '\\'")
                params.append(pattern)
        for key in ("author", "series", "genre", "tag"):
            if query.get(key, [""])[0]:
                conditions.append(
                    "EXISTS (SELECT 1 FROM book_facets f WHERE f.owner=b.owner "
                    "AND f.book=b.id AND f.kind=? AND f.value=?)"
                )
                value = normalized(query[key][0][:500])
                if key == "genre":
                    value = GENRE_ALIASES.get(value, value)
                params.extend((key, value))
        for key, kind in (("genreGroup", "genre"), ("tagGroup", "tag")):
            if query.get(key, [""])[0]:
                conditions.append(
                    "EXISTS (SELECT 1 FROM book_facets f WHERE f.owner=b.owner "
                    "AND f.book=b.id AND f.kind=? AND f.group_id=?)"
                )
                params.extend((kind, query[key][0][:80]))
        letter = normalized(query.get("letter", [""])[0])[:1]
        if letter:
            field = {"author": "b.author", "series": "b.series"}.get(scope, "b.title")
            conditions.append(f"substr(normalized({field}),1,1)=?")
            params.append(letter)
        shelf = query.get("shelf", [""])[0]
        if shelf == "favorite":
            conditions.append(
                "EXISTS (SELECT 1 FROM favorite_books f WHERE f.owner=b.owner AND f.book=b.id)"
            )
        elif shelf == "reading":
            conditions.append(
                "EXISTS (SELECT 1 FROM reading_activity a WHERE a.owner=b.owner AND a.book=b.id)"
            )
        elif shelf:
            conditions.append(
                "EXISTS (SELECT 1 FROM reading r WHERE r.owner=b.owner "
                "AND r.book=b.id AND r.shelf=?)"
            )
            params.append(shelf)
        personal = query.get("personal", [""])[0] == "1" or bool(personal_collection)
        if personal:
            conditions.append(
                "EXISTS (SELECT 1 FROM personal_library p WHERE p.owner=b.owner AND p.book=b.id)"
            )
        page = max(1, min(100000, int(query.get("page", ["1"])[0])))
        sort = query.get("sort", ["recent"])[0]
        order = {
            "title": "normalized(b.title),CAST(b.id AS INTEGER)",
            "title_desc": "normalized(b.title) DESC,CAST(b.id AS INTEGER)",
            "author": "normalized(b.author),normalized(b.title),b.id",
            "series": "normalized(b.series),"
            "CAST(json_extract(b.record,'$.seriesNumber') AS REAL),b.title,b.id",
            "recent": "CAST(b.id AS INTEGER) DESC",
            "random": "random()",
        }.get(sort, "b.title,b.id")
        if personal and sort == "recent":
            order = "(SELECT added FROM personal_library p WHERE p.owner=b.owner "
            order += "AND p.book=b.id) DESC,CAST(b.id AS INTEGER) DESC"
        elif shelf == "reading":
            # This view is reading history, not a manually assigned status shelf.
            order = "(SELECT last_read FROM reading_activity a WHERE a.owner=b.owner "
            order += "AND a.book=b.id) DESC,CAST(b.id AS INTEGER) DESC"
        where = " AND ".join(conditions)
        if arrivals:
            order = "CAST(b.id AS INTEGER) DESC,b.id DESC"
        revision = self.catalog_status(session, refresh=False)["revision"]
        with self.db() as db:
            total = db.execute(f"SELECT count(*) FROM books b WHERE {where}", params).fetchone()[0]
            rows = db.execute(
                "SELECT b.record,r.chapter,r.offset,r.shelf FROM books b "
                "LEFT JOIN reading r ON r.owner=b.owner AND r.book=b.id "
                f"WHERE {where} ORDER BY {order} LIMIT 50 OFFSET ?",
                [*params, (page - 1) * 50],
            ).fetchall()
        return {
            "books": [
                decorate_record(json.loads(row[0]))
                | {
                    "reading": {
                        "chapter": row[1] or 0,
                        "offset": row[2] or 0,
                        "shelf": row[3] or "",
                    }
                }
                for row in rows
            ],
            "total": total,
            "page": page,
            "more": page * 50 < total,
            "indexStatus": self.index_status.get(session.owner, "ready"),
            "catalogRevision": revision,
        }

    def catalog_status(self, session, refresh=True):
        if refresh:
            self.ensure_index(session)
        with self.db() as db:
            row = db.execute(
                "SELECT refreshed FROM catalog_state WHERE owner=?", (session.owner,)
            ).fetchone()
        return {
            "revision": row[0] if row else 0,
            "status": self.index_status.get(session.owner, "ready"),
        }

    def facets(self, owner, query=None):
        query = query or {}
        kind = query.get("kind", ["genre"])[0]
        if kind not in {"author", "series", "genre", "tag"}:
            raise WebError(400, "Неизвестный раздел поиска")
        group = query.get("group", [""])[0][:80]
        term = normalized(query.get("q", [""])[0][:200])
        letter = normalized(query.get("letter", [""])[0])[:1]
        page = max(1, min(100000, int(query.get("page", ["1"])[0])))
        categories = (
            GENRE_CATEGORIES if kind == "genre" else TAG_CATEGORIES if kind == "tag" else {}
        )
        shared = " AND NOT EXISTS (SELECT 1 FROM uploaded_books u WHERE u.owner=book_facets.owner AND u.book=book_facets.book)"
        with self.db() as db:
            groups = [
                {"value": r[0], "label": categories.get(r[0], r[0]), "count": r[1], "terms": r[2]}
                for r in db.execute(
                    "SELECT group_id,count(DISTINCT book),count(DISTINCT value) FROM book_facets "
                    "WHERE owner=? AND kind=? AND group_id!=''" + shared + " GROUP BY group_id",
                    (owner, kind),
                )
            ]
            where, params = "owner=? AND kind=?" + shared, [owner, kind]
            if group:
                where += " AND group_id=?"
                params.append(group)
            rows = db.execute(
                "SELECT value,min(label) AS label,min(search) AS search,count(*) AS count "
                f"FROM book_facets WHERE {where} GROUP BY value",
                params,
            ).fetchall()
        # Normalized contains matching works for Cyrillic, ё and multiple words;
        # directories are paged, never truncated to the first 2,000/5,000 names.
        category_match = term and term in normalized(categories.get(group, ""))
        items = [
            dict(r) for r in rows if category_match or all(w in r["search"] for w in term.split())
        ]
        letters = sorted({r["search"][0].upper() for r in items if r["search"]})
        if letter:
            items = [r for r in items if r["search"].startswith(letter)]
        if query.get("sort", ["alpha"])[0] == "count":
            items.sort(key=lambda r: (-r["count"], r["search"], r["value"]))
        else:
            items.sort(key=lambda r: (r["search"], r["value"]))
        total = len(items)
        items = items[(page - 1) * 40 : page * 40]
        for item in items:
            item.pop("search")
        return {
            "kind": kind,
            "items": items,
            "total": total,
            "page": page,
            "more": page * 40 < total,
            "letters": letters,
            "groups": sorted(groups, key=lambda g: normalized(g["label"])),
        }

    def devices(self, owner):
        with self.db() as db:
            rows = [
                dict(r)
                for r in db.execute(
                    "SELECT id,name,last_seen FROM devices WHERE owner=? ORDER BY last_seen DESC",
                    (owner,),
                )
            ]
        return rows or [{"id": "default", "name": "Моя читалка", "last_seen": 0}]

    def enqueue(self, owner, book_id, device):
        book = self.book(owner, book_id)
        if device not in {d["id"] for d in self.devices(owner)}:
            raise WebError(404, "Устройство не найдено")
        now = time.time()
        with self.db() as db:
            db.execute("BEGIN IMMEDIATE")
            previous = db.execute("SELECT * FROM delivery WHERE owner=? AND device=? AND book=?", (owner, device, book_id)).fetchone()
            if previous and previous["action"] == "download" and previous["state"] in {"queued", "downloading", "delivered"}:
                return dict(previous)
            present = previous["present"] if previous else 0
            # A new intent gets a new lease ID. A late ACK for the previous
            # download/removal must never acknowledge the opposite operation.
            db.execute("DELETE FROM delivery WHERE owner=? AND device=? AND book=?", (owner, device, book_id))
            db.execute(
                """INSERT INTO delivery(owner,device,book,title,created,updated,present)
                VALUES (?,?,?,?,?,?,?)""",
                (owner, device, book_id, book["title"], now, now, present),
            )
            return dict(db.execute("SELECT * FROM delivery WHERE owner=? AND device=? AND book=?", (owner, device, book_id)).fetchone())

    def remove_from_device(self, owner, job):
        with self.db() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM delivery WHERE owner=? AND id=?", (owner, job)).fetchone()
            if not row:
                raise WebError(404, "Книга на устройстве не найдена")
            if row["action"] == "remove" and row["state"] != "failed":
                return dict(row)
            db.execute("DELETE FROM delivery WHERE owner=? AND id=?", (owner, job))
            now = time.time()
            cursor = db.execute("""INSERT INTO delivery(owner,device,book,title,created,updated,action,present)
                VALUES (?,?,?,?,?,?,'remove',?)""", (owner, row["device"], row["book"], row["title"], now, now, row["present"]))
            return dict(db.execute("SELECT * FROM delivery WHERE id=?", (cursor.lastrowid,)).fetchone())

    def deliveries(self, owner):
        with self.db() as db:
            return [
                dict(r)
                for r in db.execute(
                    "SELECT id,device,book,title,state,created,updated,error,action,present FROM delivery "
                    "WHERE owner=? AND state NOT IN ('removed','cancelled') ORDER BY updated DESC",
                    (owner,),
                )
            ]

    def poll(self, owner, device, version=1, after=0):
        if not DEVICE_RE.fullmatch(device):
            raise WebError(400, "Invalid device identifier")
        if type(after) is not int or not 0 <= after < 2**63:
            raise WebError(400, "Invalid delivery cursor")
        now = time.time()
        with self.db() as db:
            db.execute("BEGIN IMMEDIATE")
            existing = db.execute(
                "SELECT count(*) FROM devices WHERE owner=?", (owner,)
            ).fetchone()[0]
            if not existing:
                db.execute(
                    "UPDATE delivery SET device=? WHERE owner=? AND device='default'",
                    (device, owner),
                )
            if (
                existing >= 16
                and not db.execute(
                    "SELECT 1 FROM devices WHERE owner=? AND id=?", (owner, device)
                ).fetchone()
            ):
                raise WebError(409, "Device limit reached")
            db.execute(
                """INSERT INTO devices VALUES (?,?,?,?) ON CONFLICT(owner,id)
                DO UPDATE SET last_seen=excluded.last_seen""",
                (owner, device, "Читалка " + device[-4:].upper(), now),
            )
            row = db.execute(
                """SELECT * FROM delivery WHERE owner=? AND device=? AND
                (state='queued' OR (state='downloading' AND updated<?))
                AND (action='download' OR ? >= 2) AND id>? ORDER BY id LIMIT 1""",
                (owner, device, now - 1800, version, after),
            ).fetchone()
            if not row:
                return b""
            db.execute(
                "UPDATE delivery SET state='downloading',updated=? WHERE id=?", (now, row["id"])
            )
        if row["action"] == "remove":
            return f"2\t{row['id']}\topds-{row['book']}\tremove\n".encode()
        prefix = f"/reader-api/uploads/{row['book']}" if self.upload_path(owner, row["book"]) else f"{PREFIX}/{row['book']}"
        download = f"{prefix}/download" if prefix.startswith('/reader-api/uploads/') else f"/reader-api/compatible/{row['book']}/download"
        # Tiny versioned protocol: never send the catalogue to an ESP32.
        return (
            f"1\t{row['id']}\topds-{row['book']}\t"
            f"{self.origin}{download}\t"
            f"{self.origin}{prefix}/cover\n"
        ).encode()

    def acknowledge(self, owner, device, job, state, digest="", error=""):
        if state not in {"delivered", "removed", "failed", "queued"}:
            raise WebError(400, "Invalid delivery state")
        if state == "delivered" and not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise WebError(400, "Missing verified file digest")
        with self.db() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute(
                "SELECT state,action FROM delivery WHERE owner=? AND device=? AND id=?",
                (owner, device, job),
            ).fetchone()
            if not row:
                raise WebError(404, "Delivery not found")
            if state in {"delivered", "removed"} and state != ("removed" if row["action"] == "remove" else "delivered"):
                raise WebError(409, "Wrong acknowledgement action")
            if row[0] in {"delivered", "removed"}:
                return
            if row[0] != "downloading":
                raise WebError(409, "Delivery is not leased")
            db.execute(
                "UPDATE delivery SET state=?,sha256=?,error=?,updated=?, "
                "present=CASE WHEN ?='delivered' THEN 1 WHEN ?='removed' THEN 0 ELSE present END "
                "WHERE owner=? AND device=? AND id=?",
                (state, digest, error[:160], time.time(), state, state, owner, device, job),
            )

    def chapters(self, session, book_id):
        # One FB2 parse at a time: cover binaries/XML expansion can be sizable.
        with self.read_gate:
            return self._chapters(session, book_id)

    def _chapters(self, session, book_id):
        self.book(session.owner, book_id)
        key = (session.owner, book_id)
        with self.lock:
            if key in self.read_cache:
                self.read_cache.move_to_end(key)
                return self.read_cache[key]
        private = self.upload_path(session.owner, book_id)
        if private:
            data = private.read_bytes()
        else:
            with self.client.open(f"/{book_id}/download", (), session.authorization) as response:
                data = response.read(MAX_BOOK + 1)
        chapters = fb2_chapters(data)
        with self.lock:
            self.read_cache[key] = chapters
            while (
                len(self.read_cache) > 3
                or sum(len(c["html"].encode()) for book in self.read_cache.values() for c in book)
                > 32 * 1024 * 1024
            ):
                self.read_cache.popitem(last=False)
        return chapters


class WebServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, address, app):
        self.app = app
        self.slots = threading.BoundedSemaphore(24)
        super().__init__(address, WebHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class WebHandler(BaseHTTPRequestHandler):
    server_version = "OctoFox"

    def setup(self):
        super().setup()
        self.connection.settimeout(30)

    def log_message(self, *_args):
        pass

    @property
    def app(self):
        return self.server.app

    def send(self, status, value=b"", content_type="application/json; charset=utf-8", headers=None):
        data = (
            json.dumps(value, ensure_ascii=False).encode()
            if not isinstance(value, bytes)
            else value
        )
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", (headers or {}).get("Cache-Control", "no-store"))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "same-origin")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data:; media-src 'self' blob:; connect-src 'self'; frame-ancestors 'none'; "
            "base-uri 'none'; form-action 'self'",
        )
        for key, val in (headers or {}).items():
            if key.lower() != "cache-control":
                self.send_header(key, val)
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        if "application/json" not in self.headers.get("Content-Type", ""):
            raise WebError(415, "Ожидался JSON")
        length = int(self.headers.get("Content-Length", "0"))
        if not 0 < length <= 8192:
            raise WebError(413, "Слишком большой запрос")
        payload = getattr(self, "_json_payload", None)
        data = json.loads(payload if payload is not None else self.rfile.read(length))
        if not isinstance(data, dict):
            raise WebError(400, "Некорректный запрос")
        return data

    def session(self):
        cookie = SimpleCookie()
        cookie.load(self.headers.get("Cookie", ""))
        token = cookie.get("books_session")
        token = token.value if token else ""
        with self.app.lock:
            session = self.app.sessions.get(token)
        if not session or session.expires < time.time():
            raise WebError(401, "Войдите в библиотеку")
        self.app.authenticate(session.authorization)
        if self.command == "POST" and not secrets.compare_digest(
            self.headers.get("X-CSRF-Token", ""), session.csrf
        ):
            raise WebError(403, "Обновите страницу и повторите действие")
        return token, session

    def do_GET(self):  # noqa: N802
        self.dispatch()

    def do_POST(self):  # noqa: N802
        self.dispatch()

    def dispatch(self):
        try:
            self.route()
        except WebError as error:
            self.send(error.status, {"error": error.message})
        except SpeechError as error:
            self.send(error.status, {"error": error.message})
        except UpstreamFailure as error:
            self.send(
                401 if error.status in {401, 403} else 502,
                {
                    "error": "Неверный логин или пароль библиотеки"
                    if error.status in {401, 403}
                    else "Библиотека временно недоступна"
                },
            )
        except (ValueError, TypeError, KeyError):
            self.send(400, {"error": "Некорректный запрос"})
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception:
            LOGGER.exception("Books request failed")
            self.send(500, {"error": "Не удалось выполнить запрос. Попробуйте ещё раз"})

    def route(self):
        path = urlsplit(self.path).path
        query = parse_qs(urlsplit(self.path).query)
        get = self.command == "GET"
        # Consume only bounded JSON bodies before an early auth/origin rejection.
        # Closing a socket with unread request bytes can replace the HTTP error
        # with a TCP reset on Windows. Upload bytes remain streamed separately.
        self._json_payload = None
        if not get and self.headers.get_content_type() == "application/json":
            length = int(self.headers.get("Content-Length", "0"))
            if self.headers.get("Transfer-Encoding"):
                raise WebError(400, "Некорректный запрос")
            if 0 < length <= 8192:
                self._json_payload = self.rfile.read(length)
                if len(self._json_payload) != length:
                    raise WebError(400, "Запрос передан не полностью")
        if get and path == "/healthz":
            return self.send(200, b"ok\n", "text/plain")
        if path.startswith('/companion-api/'):
            return self.app.companion.route(self, path)
        if get and path in {'/companion', '/companion/'}:
            return self.send(200, (STATIC / 'companion.html').read_bytes(), 'text/html; charset=utf-8')
        if get and path == '/connection-check':
            self.app.network.request_origin(self.headers)
            return self.send(200, (STATIC / 'connection-check.html').read_bytes(), 'text/html; charset=utf-8')
        if path.startswith("/fonts/"):
            if not get or path not in {
                "/fonts/cormorant-garamond-600-cyrillic-v1.woff2",
                "/fonts/cormorant-garamond-600-latin-v1.woff2",
            }:
                raise WebError(404, "Not found")
            return self.send(200, (STATIC / path[1:]).read_bytes(), "font/woff2",
                             headers={"Cache-Control": "public, max-age=31536000, immutable"})
        if path.startswith("/cover-art/"):
            # Only public, versioned decorative assets; never accept arbitrary paths.
            if not get or not re.fullmatch(r"/cover-art/v1/(0[1-9]|1[0-6])\.webp", path):
                raise WebError(404, "Not found")
            return self.send(200, (STATIC / path[1:]).read_bytes(), "image/webp",
                             headers={"Cache-Control": "public, max-age=31536000, immutable"})
        if get and path in {
            "/",
            "/index.html",
            "/app.js",
            "/voice-reader.js",
            "/server-voice.js",
            "/page-voice.js",
            "/bookmarks.js",
            "/personal-library.js",
            "/styles.css",
            "/manifest.webmanifest",
            "/icon.svg",
            "/korean-elves.png",
            "/companion.js",
            "/companion.css",
            "/companion-network.js",
            "/connection-check.js",
        }:
            name = "index.html" if path == "/" else path[1:]
            mime = {
                ".html": "text/html; charset=utf-8",
                ".js": "text/javascript; charset=utf-8",
                ".css": "text/css; charset=utf-8",
                ".webmanifest": "application/manifest+json",
                ".svg": "image/svg+xml",
                ".png": "image/png",
            }
            return self.send(200, (STATIC / name).read_bytes(), mime[Path(name).suffix])
        if path.startswith("/reader-api/device/"):
            self.app.throttle("device:" + self.client_address[0], limit=120)
            owner = self.app.authenticate(self.headers.get("Authorization", ""))
            device = query.get("device", [""])[0]
            if not DEVICE_RE.fullmatch(device):
                raise WebError(400, "Invalid device identifier")
            if get and path == "/reader-api/device/next":
                value = self.app.poll(owner, device, int(query.get("v", ["1"])[0]),
                                      int(query.get("after", ["0"])[0]))
                return self.send(200 if value else 204, value, "text/plain; charset=utf-8")
            if not get and path == "/reader-api/device/ack":
                data = self.body()
                self.app.acknowledge(
                    owner,
                    device,
                    int(data["job"]),
                    data["state"],
                    data.get("sha256", ""),
                    data.get("error", ""),
                )
                return self.send(200, {"ok": True})
            raise WebError(404, "Not found")
        upload_asset = re.fullmatch(r"/reader-api/uploads/(\d+)/(download|cover)", path)
        compatible = re.fullmatch(r'/reader-api/compatible/(\d+)/download', path)
        if get and compatible:
            authorization = self.headers.get('Authorization', '')
            session = (Session(self.app.authenticate(authorization), authorization, '', time.time() + 60)
                       if authorization else self.session()[1])
            if not self.app.download_gate.acquire(blocking=False):
                raise WebError(429, 'Попробуйте через несколько секунд')
            try:
                data, disposition = self.app.download_book(session, compatible[1])
                return self.send(200, data, 'application/x-fictionbook+xml',
                                 headers={'Content-Disposition': disposition})
            finally:
                self.app.download_gate.release()
        if get and upload_asset:
            owner = (self.app.authenticate(self.headers["Authorization"])
                     if self.headers.get("Authorization") else self.session()[1].owner)
            book_id, action = upload_asset.groups()
            private = self.app.upload_path(owner, book_id)
            if not private or not private.is_file():
                raise WebError(404, "Файл не найден")
            if action == "cover":
                data, mime = self.app.upload_cover(owner, book_id)
                return self.send(200, data, mime)
            if not self.app.download_gate.acquire(blocking=False):
                raise WebError(429, "Попробуйте через несколько секунд")
            try:
                return self.send(200, private.read_bytes(), "application/x-fictionbook+xml",
                                 headers={"Content-Disposition": self.app.file_disposition(self.app.book(owner, book_id))})
            finally:
                self.app.download_gate.release()
        if not get:
            origin = self.headers.get("Origin")
            if origin:
                self.app.network.request_origin(self.headers, require_origin=True)
        if not get and path == "/reader-api/login":
            self.app.throttle("login:" + self.client_address[0])
            data = self.body()
            token, session = self.app.login(
                str(data.get("username", "")), str(data.get("password", ""))
            )
            request_origin = self.app.network.request_origin(self.headers)
            secure = "; Secure" if request_origin.startswith("https:") else ""
            return self.send(
                200,
                {"username": session.owner, "csrf": session.csrf},
                headers={
                    "Set-Cookie": f"books_session={token}; HttpOnly; SameSite=Lax; Path=/; "
                    f"Max-Age={SESSION_SECONDS}{secure}"
                },
            )
        token, session = self.session()
        if get and path == "/reader-api/speech":
            self.app.speech.warm_library(session)
            return self.send(200, self.app.speech.info(session.owner))
        speech = re.fullmatch(r"/reader-api/speech/streams/([a-f0-9-]{36})/(status|audio.m3u8|page.wav|stop|\d+\.(?:ts|wav))", path)
        if speech:
            identity, action = speech.groups()
            if not get and action == 'stop':
                return self.send(200, self.app.speech.stop(session.owner, identity))
            if not get:
                raise WebError(405, 'Метод не поддерживается')
            if action == 'status':
                return self.send(200, self.app.speech.status(session.owner, identity))
            if action == 'audio.m3u8':
                data = self.app.speech.playlist(session.owner, identity)
                self.session()  # Logout while waiting must not release private audio.
                return self.send(200, data, 'application/vnd.apple.mpegurl')
            media = re.fullmatch(r'(\d+)\.(ts|wav)', action)
            if media or action == 'page.wav':
                data = self.app.speech.page_audio(session.owner, identity) if action == 'page.wav' else self.app.speech.media(session.owner, identity, int(media[1]), media[2])
                self.session()
                headers = {'Accept-Ranges': 'bytes'}
                status = 200
                value = self.headers.get('Range')
                if value:
                    span = re.fullmatch(r'bytes=(\d*)-(\d*)', value)
                    if not span or not any(span.groups()):
                        raise WebError(416, 'Некорректный диапазон')
                    start = int(span[1]) if span[1] else max(0, len(data) - int(span[2]))
                    end = min(len(data)-1, int(span[2])) if span[1] and span[2] else len(data)-1
                    if start > end or start >= len(data):
                        return self.send(416, headers={'Content-Range': f'bytes */{len(data)}'})
                    headers['Content-Range'] = f'bytes {start}-{end}/{len(data)}'
                    data, status = data[start:end+1], 206
                return self.send(status, data, 'video/mp2t' if media and media[2] == 'ts' else 'audio/wav', headers)
            raise WebError(404, 'Не найдено')
        if not get and path == "/reader-api/uploads":
            from octofox_library.web_uploads import MAX_UPLOAD
            self.app.throttle("upload:" + session.owner, limit=60)
            length = int(self.headers.get("Content-Length", "0"))
            if self.headers.get("Transfer-Encoding") or not 256 <= length <= MAX_UPLOAD:
                raise WebError(413, "Выбери FB2 или EPUB размером до 16 МБ")
            if self.headers.get_content_type() not in {"application/octet-stream", "application/x-fictionbook+xml", "application/epub+zip"}:
                raise WebError(415, "Загружать можно файлы FB2 и EPUB")
            if not self.app.upload_gate.acquire(blocking=False):
                raise WebError(429, "Уже загружается другая книга. Попробуй чуть позже")
            try:
                self.connection.settimeout(45)
                payload = self.rfile.read(length)
                if len(payload) != length:
                    raise WebError(400, "Файл передан не полностью")
                filename = self.headers.get('X-Upload-Filename', '')
                if len(filename) > 4096:
                    raise WebError(400, 'Слишком длинное имя файла')
                from urllib.parse import unquote
                return self.send(201, self.app.upload_book(session.owner, payload,
                    filename=unquote(filename, errors='strict'), modified=int(self.headers.get('X-Upload-Modified', '0'))))
            finally:
                self.app.upload_gate.release()
        if not get and path == "/reader-api/logout":
            with self.app.lock:
                self.app.sessions.pop(token, None)
            return self.send(
                200,
                {"ok": True},
                headers={"Set-Cookie": "books_session=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0"},
            )
        if get and path == "/reader-api/me":
            return self.send(
                200,
                {
                    "username": session.owner,
                    "csrf": session.csrf,
                    "devices": self.app.devices(session.owner),
                },
            )
        if get and path == "/reader-api/books":
            return self.send(200, self.app.catalog(session, query))
        if get and path == "/reader-api/collections":
            return self.send(200, self.app.collections(session))
        if path == '/reader-api/personal-collections':
            return self.send(200, self.app.personal_collections(session.owner, None if get else self.body()))
        if get and path == "/reader-api/catalog-status":
            return self.send(200, self.app.catalog_status(session))
        if get and path == "/reader-api/facets":
            return self.send(200, self.app.facets(session.owner, query))
        if get and path == "/reader-api/devices":
            return self.send(200, self.app.devices(session.owner))
        if get and path == "/reader-api/queue":
            return self.send(200, self.app.deliveries(session.owner))
        if not get and path == "/reader-api/queue":
            data = self.body()
            return self.send(
                200, self.app.enqueue(session.owner, str(data["book"]), str(data["device"]))
            )
        match = re.fullmatch(r"/reader-api/queue/(\d+)/(cancel|retry|remove)", path)
        if not get and match:
            if match[2] in {"cancel", "remove"}:
                return self.send(200, self.app.remove_from_device(session.owner, int(match[1])))
            with self.app.db() as db:
                row = db.execute(
                    "SELECT * FROM delivery WHERE owner=? AND id=?", (session.owner, match[1])
                ).fetchone()
                if not row:
                    raise WebError(404, "Загрузка не найдена")
            result = (self.app.remove_from_device(session.owner, row["id"]) if row["action"] == "remove"
                      else self.app.enqueue(session.owner, row["book"], row["device"]))
            return self.send(200, result)
        match = re.fullmatch(
            r"/reader-api/books/(\d+)(?:/(cover|read|state|library|favorite|download|speech|bookmarks|personal-collections))?", path
        )
        if not match:
            raise WebError(404, "Не найдено")
        book_id, action = match.groups()
        book = self.app.book(session.owner, book_id)
        if get and action is None:
            return self.send(200, book)
        if action == 'bookmarks':
            return self.send(200, self.app.bookmarks(session.owner, book_id, None if get else self.body()))
        if action == 'personal-collections':
            return self.send(200, self.app.book_collections(session.owner, book_id, None if get else self.body()))
        if not get and action == "library":
            result = self.app.save_book(session.owner, book_id, self.body().get("saved"))
            if result['inLibrary']:
                self.app.speech.request(session, book_id)
            else:
                self.app.speech.forget(session.owner, book_id)
            return self.send(200, result)
        if not get and action == 'speech':
            return self.send(202, self.app.speech.prepare(session, book_id, self.body()))
        if not get and action == "favorite":
            return self.send(
                200, self.app.favorite_book(session.owner, book_id, self.body().get("favorite"))
            )
        if get and action == "download":
            if not self.app.download_gate.acquire(blocking=False):
                raise WebError(429, "Загружаются другие книги. Попробуйте через несколько секунд")
            try:
                data, disposition = self.app.download_book(session, book_id)
                return self.send(
                    200,
                    data,
                    "application/x-fictionbook+xml",
                    headers={"Content-Disposition": disposition},
                )
            finally:
                # Keep the bound until the slow client has received the file.
                self.app.download_gate.release()
        if get and action == "cover":
            if self.app.upload_path(session.owner, book_id):
                data, content_type = self.app.upload_cover(session.owner, book_id)
                return self.send(200, data, content_type)
            with self.app.client.open(f"/{book_id}/cover", (), session.authorization) as response:
                content_type = response.headers.get_content_type()
                data = response.read(4 * 1024 * 1024 + 1)
            if (
                content_type not in {"image/jpeg", "image/png", "image/webp"}
                or len(data) > 4 * 1024 * 1024
                # BookLore returns its bundled generic JPEG with HTTP 200.
                # Match exact bytes only: never guess based on size/colour/text.
                or (len(data) == 19071 and hashlib.sha256(data).hexdigest()
                    == "193f0ad487653fd51861b3f3a17a8ab9a4054bdcdf3e0e5049ef4c4c71cfe957")
            ):
                raise WebError(404, "Нет обложки")
            return self.send(200, data, content_type)
        if get and action == "read":
            chapters = self.app.chapters(session, book_id)
            if "start" in query:
                # Reuse the authenticated, sanitized chapter HTML in bounded batches.
                start = max(0, min(len(chapters), int(query["start"][0])))
                limit = max(1, min(32, int(query.get("limit", ["16"])[0])))
                items, size = [], 0
                for number in range(start, min(len(chapters), start + limit)):
                    html = chapters[number]["html"]
                    length = len(html.encode("utf-8"))
                    if items and size + length > 256 * 1024:
                        break
                    items.append({"chapter": number, "html": html})
                    size += length
                return self.send(200, {"items": items, "next": start + len(items), "total": len(chapters)})
            chapter = max(0, min(len(chapters) - 1, int(query.get("chapter", ["0"])[0])))
            return self.send(
                200,
                {
                    "chapter": chapter,
                    "chapters": [c["title"] for c in chapters],
                    **chapters[chapter],
                },
            )
        if not get and action == "state":
            result = self.app.save_state(session.owner, book_id, self.body())
            self.app.speech.request(session, book_id, active=True)
            return self.send(200, result)
        raise WebError(405, "Метод не поддерживается")


def main():
    # Disable redirects before using authenticated upstream requests.
    import urllib.request

    urllib.request.install_opener(build_opener(NoRedirect()))
    logging.basicConfig(level=logging.INFO)
    app = LibraryWeb(
        Path(os.environ.get("OCTOFOX_DB", "./data/library.sqlite3")),
        os.environ.get("OCTOFOX_UPSTREAM", "http://booklore:6060/api/v1/opds"),
        os.environ.get("OCTOFOX_ORIGIN", "http://localhost:8080"),
    )
    app.speech.start()
    WebServer(
        (
            os.environ.get("OCTOFOX_BIND", "0.0.0.0"),
            int(os.environ.get("OCTOFOX_PORT", "8080")),
        ),
        app,
    ).serve_forever()


if __name__ == "__main__":
    main()
