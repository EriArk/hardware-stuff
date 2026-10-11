"""Account-scoped external catalogs and a durable, single-consumer import queue."""
import base64
import hashlib
import io
import json
import logging
import secrets
import threading
import time
import zipfile
from urllib.parse import urlsplit

from .external_opds import Fetcher, parse_feed, valid_url, search_url, expand_search
from .web_errors import WebError
from .web_uploads import MAX_UPLOAD, image_type


class Cancelled(Exception):
    pass


class ExternalCatalogs:
    def __init__(self, app):
        self.app = app
        self.fetcher = Fetcher()
        self.stop_event = threading.Event()
        self.wake = threading.Event()
        self.thread = None
        self.gate = threading.Lock()
        with app.db() as db:
            db.executescript('''
                CREATE TABLE IF NOT EXISTS external_catalogs (
                    owner TEXT, id TEXT, name TEXT, url TEXT, authorization TEXT,
                    PRIMARY KEY(owner,id), UNIQUE(owner,url));
                CREATE TABLE IF NOT EXISTS external_items (
                    owner TEXT, catalog TEXT, id TEXT, record TEXT, updated REAL,
                    PRIMARY KEY(owner,catalog,id));
                CREATE TABLE IF NOT EXISTS external_downloads (
                    id TEXT PRIMARY KEY, owner TEXT, catalog TEXT, item TEXT, title TEXT,
                    record TEXT, state TEXT, received INTEGER DEFAULT 0, total INTEGER DEFAULT 0,
                    book TEXT DEFAULT '', error TEXT DEFAULT '', created REAL, updated REAL,
                    UNIQUE(owner,catalog,item));
                CREATE INDEX IF NOT EXISTS external_jobs_owner ON external_downloads(owner,created);
            ''')

    def start(self):
        if self.thread:
            return
        with self.app.db() as db:
            db.execute("UPDATE external_downloads SET state='queued',received=0,total=0 WHERE state IN ('downloading','importing')")
        self.stop_event.clear()
        self.thread = threading.Thread(target=self.run, name='opds-import', daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.wake.set()
        if self.thread:
            self.thread.join(timeout=20)

    def run(self):
        while not self.stop_event.is_set():
            try:
                if self.run_one():
                    continue
            except Exception:
                logging.exception('External import worker failed')
            self.wake.wait(3)
            self.wake.clear()

    def catalog(self, owner, identity):
        with self.app.db() as db:
            row = db.execute('SELECT * FROM external_catalogs WHERE owner=? AND id=?', (owner, identity)).fetchone()
        if not row:
            raise WebError(404, 'Catalog not found.')
        return dict(row)

    def catalogs(self, owner):
        with self.app.db() as db:
            rows = db.execute('SELECT id,name,url FROM external_catalogs WHERE owner=? ORDER BY name', (owner,)).fetchall()
        return {'catalogs': [dict(r) for r in rows]}

    def save(self, owner, data):
        name = str(data.get('name', '')).strip()
        url = valid_url(data.get('url', ''))
        username, password = str(data.get('username', '')), str(data.get('password', ''))
        if not name or len(name) > 100 or len(username) > 200 or ':' in username or len(password) > 512 or (password and not username):
            raise WebError(400, 'Enter a catalog name and valid credentials.')
        if (username or password) and urlsplit(url).scheme != 'https':
            raise WebError(400, 'Use HTTPS for a catalog with a password.')
        auth = 'Basic ' + base64.b64encode(f'{username}:{password}'.encode()).decode() if username else ''
        payload, actual, _ = self.fetcher.fetch(url, root=url, authorization=auth)
        parse_feed(payload, actual)
        with self.app.db() as db:
            db.execute('BEGIN IMMEDIATE')
            existing = db.execute('SELECT id FROM external_catalogs WHERE owner=? AND url=?', (owner, url)).fetchone()
            if not existing and db.execute('SELECT count(*) FROM external_catalogs WHERE owner=?', (owner,)).fetchone()[0] >= 20:
                raise WebError(400, 'You can connect up to 20 catalogs.')
            identity = existing[0] if existing else secrets.token_hex(12)
            db.execute('INSERT OR REPLACE INTO external_catalogs VALUES (?,?,?,?,?)', (owner, identity, name, url, auth))
        return {'id': identity}

    def remove(self, owner, identity):
        self.catalog(owner, identity)
        with self.gate, self.app.db() as db:
            db.execute("UPDATE external_downloads SET state='cancelled',updated=? WHERE owner=? AND catalog=? AND state IN ('queued','downloading')", (time.time(), owner, identity))
            db.execute('DELETE FROM external_catalogs WHERE owner=? AND id=?', (owner, identity))
            db.execute('DELETE FROM external_items WHERE owner=? AND catalog=?', (owner, identity))
        return {'ok': True}

    def fetch(self, catalog, url, **options):
        return self.fetcher.fetch(url, root=catalog['url'], authorization=catalog['authorization'], **options)

    def browse(self, owner, identity, url='', terms=''):
        catalog = self.catalog(owner, identity)
        payload, actual, _ = self.fetch(catalog, url or catalog['url'])
        page = parse_feed(payload, actual)
        if terms:
            search = page['search']
            if not search:
                raise WebError(400, 'This catalog does not provide a supported search.')
            if 'opensearchdescription' in search.get('type', ''):
                description, base, _ = self.fetch(catalog, search['href'])
                destination = search_url(description, base, terms)
            else:
                destination = expand_search(search['href'], terms)
            payload, actual, _ = self.fetch(catalog, destination)
            page = parse_feed(payload, actual)
        with self.app.db() as db:
            for item in page['items']:
                item['id'] = hashlib.sha256(item['identity'].encode()).hexdigest()[:32]
                db.execute('INSERT OR REPLACE INTO external_items VALUES (?,?,?,?,?)',
                           (owner, identity, item['id'], json.dumps(item), time.time()))
            # These are only browsing metadata; queued jobs keep their own immutable snapshot.
            db.execute('DELETE FROM external_items WHERE owner=? AND updated<?', (owner, time.time() - 30 * 86400))
            db.execute('DELETE FROM external_items WHERE rowid IN (SELECT rowid FROM external_items WHERE owner=? ORDER BY updated DESC LIMIT -1 OFFSET 10000)', (owner,))
        page['search'] = bool(page['search'])
        for item in page['items']:
            item['available'] = bool(item['downloads'] or item['detail'])
            item['formats'] = list(dict.fromkeys(x['format'] for x in item.pop('downloads')))
            item['cover'] = bool(item['cover'])
            item.pop('identity', None)
            item.pop('detail', None)
        return page

    def item(self, owner, catalog, identity):
        with self.app.db() as db:
            row = db.execute('SELECT record FROM external_items WHERE owner=? AND catalog=? AND id=?', (owner, catalog, identity)).fetchone()
        if not row:
            raise WebError(404, 'Refresh the catalog page and select the book again.')
        return json.loads(row[0])

    def cover(self, owner, catalog_id, identity):
        catalog = self.catalog(owner, catalog_id)
        item = self.item(owner, catalog_id, identity)
        if not item['cover']:
            raise WebError(404, 'Cover not found.')
        payload, _, _ = self.fetch(catalog, item['cover'], limit=2 * 1024 * 1024)
        mime = image_type(payload)
        if not mime:
            raise WebError(404, 'Cover not found.')
        return payload, mime

    def jobs(self, owner):
        with self.app.db() as db:
            rows = db.execute('SELECT id,catalog,item,title,state,received,total,book,error,created FROM external_downloads WHERE owner=? ORDER BY created DESC LIMIT 1000', (owner,)).fetchall()
        return {'downloads': [dict(row) for row in rows]}

    def enqueue(self, owner, catalog_id, identity):
        self.catalog(owner, catalog_id)
        item = self.item(owner, catalog_id, identity)
        if not item['downloads'] and not item['detail']:
            raise WebError(400, 'This entry has no supported free EPUB or FB2 download.')
        with self.app.db() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT id FROM external_downloads WHERE owner=? AND catalog=? AND item=?', (owner, catalog_id, identity)).fetchone()
            if row:
                return {'id': row[0]}
            if db.execute('SELECT count(*) FROM external_downloads WHERE owner=?', (owner,)).fetchone()[0] >= 1000:
                raise WebError(400, 'Clear completed downloads before adding more books.')
            job = secrets.token_hex(12)
            now = time.time()
            db.execute('INSERT INTO external_downloads (id,owner,catalog,item,title,record,state,created,updated) VALUES (?,?,?,?,?,?,\'queued\',?,?)',
                       (job, owner, catalog_id, identity, item['title'], json.dumps(item), now, now))
        self.wake.set()
        return {'id': job}

    def action(self, owner, identity, action):
        with self.gate, self.app.db() as db:
            row = db.execute('SELECT * FROM external_downloads WHERE owner=? AND id=?', (owner, identity)).fetchone()
            if not row:
                raise WebError(404, 'Download not found.')
            if action == 'cancel' and row['state'] in ('queued', 'downloading'):
                db.execute("UPDATE external_downloads SET state='cancelled',updated=? WHERE id=?", (time.time(), identity))
            elif action == 'retry' and row['state'] in ('failed', 'cancelled'):
                self.catalog(owner, row['catalog'])
                db.execute("UPDATE external_downloads SET state='queued',error='',received=0,total=0,updated=? WHERE id=?", (time.time(), identity))
            elif action == 'clear' and row['state'] in ('complete', 'failed', 'cancelled'):
                db.execute('DELETE FROM external_downloads WHERE id=?', (identity,))
            else:
                raise WebError(409, 'The download has changed. Refresh its status.')
        self.wake.set()
        return {'ok': True}

    def run_one(self):
        with self.app.db() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute("SELECT * FROM external_downloads WHERE state='queued' ORDER BY created LIMIT 1").fetchone()
            if not row:
                return False
            job = dict(row)
            db.execute("UPDATE external_downloads SET state='downloading',updated=? WHERE id=?", (time.time(), job['id']))
        try:
            self.app.companion.accounts.guard(job['owner'])
            catalog = self.catalog(job['owner'], job['catalog'])
            item = json.loads(job['record'])

            def progress(received, total):
                if self.stop_event.is_set():
                    raise Cancelled()
                with self.app.db() as db:
                    state = db.execute('SELECT state FROM external_downloads WHERE id=?', (job['id'],)).fetchone()
                    if not state or state[0] != 'downloading':
                        raise Cancelled()
                    db.execute('UPDATE external_downloads SET received=?,total=?,updated=? WHERE id=?', (received, total, time.time(), job['id']))

            progress(0, 0)
            if not item['downloads']:
                payload, actual, _ = self.fetch(catalog, item['detail'], progress=progress)
                candidates = [i for i in parse_feed(payload, actual)['items'] if i['downloads']]
                if not candidates:
                    raise WebError(400, 'This entry has no supported free EPUB or FB2 download.')
                item = candidates[0]
            download = sorted(item['downloads'], key=lambda d: ('FB2', 'EPUB', 'FB2 ZIP').index(d['format']))[0]
            payload, _, _ = self.fetch(catalog, download['url'], limit=MAX_UPLOAD, progress=progress)
            if download['format'] == 'FB2 ZIP':
                with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                    entries = [f for f in archive.infolist() if f.filename.lower().endswith('.fb2') and not f.is_dir()]
                    if len(entries) != 1 or entries[0].file_size > MAX_UPLOAD:
                        raise WebError(400, 'The archive must contain one FB2 book within the upload limit.')
                    with archive.open(entries[0]) as source:
                        payload = source.read(MAX_UPLOAD + 1)
            # Serialize cancellation vs. the final import. Importing is not cancellable:
            # the book is already being atomically committed to the ordinary library.
            with self.gate:
                progress(len(payload), len(payload))
                self.app.companion.accounts.guard(job['owner'])
                with self.app.db() as db:
                    db.execute("UPDATE external_downloads SET state='importing' WHERE id=?", (job['id'],))
                result = self.app.upload_book(job['owner'], payload)
                with self.app.db() as db:
                    db.execute("UPDATE external_downloads SET state='complete',book=?,error='',updated=? WHERE id=?", (str(result['id']), time.time(), job['id']))
        except Cancelled:
            with self.app.db() as db:
                db.execute("UPDATE external_downloads SET state='queued',received=0,total=0 WHERE id=? AND state='downloading'", (job['id'],))
        except Exception:
            # Never persist URLs, credentials, response bodies or parser tracebacks.
            with self.app.db() as db:
                db.execute("UPDATE external_downloads SET state='failed',error='import_failed',updated=? WHERE id=? AND state IN ('downloading','importing')", (time.time(), job['id']))
        return True
