"""Bounded, retry-safe exchange of personal shelves with offline readers."""
import hashlib
import json
import re
import time

from octofox_library.web_catalog import normalized
from octofox_library.web_errors import WebError


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')


class ReaderCollectionsMixin:
    @staticmethod
    def profile_sync_enabled(db, owner, device):
        row = db.execute('SELECT enabled FROM reader_profile_sync WHERE owner=? AND device=?', (owner, device)).fetchone()
        return row is None or bool(row[0])

    def set_profile_sync(self, owner, device, data):
        if not isinstance(data, dict) or type(data.get('enabled')) is not bool:
            raise WebError(400, 'Expected profile synchronization checkbox state')
        if device not in {d['id'] for d in self.devices(owner)}:
            raise WebError(404, 'Device not found')
        with self.db() as db:
            db.execute('INSERT INTO reader_profile_sync VALUES (?,?,?) ON CONFLICT(owner,device) DO UPDATE SET enabled=excluded.enabled',
                       (owner, device, int(data['enabled'])))
        return {'profile_sync': data['enabled']}

    def reconcile_reader_profile(self, db, owner, device, now):
        """Apply the profile snapshot only at the start of an explicit v2 sync pass."""
        if not self.profile_sync_enabled(db, owner, device):
            return
        wanted = {r['book']: r['title'] for r in db.execute('''
            SELECT p.book,b.title FROM (
                SELECT book FROM personal_library WHERE owner=?
                UNION SELECT book FROM favorite_books WHERE owner=?
                UNION SELECT book FROM personal_collection_books WHERE owner=?
            ) p JOIN books b ON b.owner=? AND b.id=p.book''', (owner, owner, owner, owner))}
        current = {r['book']: r for r in db.execute('SELECT * FROM delivery WHERE owner=? AND device=?', (owner, device))}
        for book in sorted(wanted.keys() | current.keys()):
            previous = current.get(book)
            download = book in wanted
            action = 'download' if download else 'remove'
            if previous and previous['action'] == action and previous['state'] not in ('cancelled', 'failed'):
                continue
            if not download and (not previous or previous['state'] in ('removed', 'cancelled')):
                continue
            # Replace the lease ID so an old ACK cannot complete an opposite intent.
            db.execute('DELETE FROM delivery WHERE owner=? AND device=? AND book=?', (owner, device, book))
            db.execute('''INSERT INTO delivery(owner,device,book,title,created,updated,present,action)
                VALUES (?,?,?,?,?,?,?,?)''', (owner, device, book, wanted[book] if download else previous['title'],
                                             now, now, previous['present'] if previous else 0, action))

    def reader_collections(self, owner, device, data=None, offset=0):
        if data is not None:
            return self._reader_collection_edit(owner, device, data)
        if type(offset) is not int or not 0 <= offset <= 30000:
            raise WebError(400, 'Invalid collection cursor')
        with self.db() as db:
            # One read snapshot: names and membership must describe the same revision.
            db.execute('BEGIN')
            salt = db.execute("SELECT value FROM reader_collection_meta WHERE key='identity'").fetchone()[0]
            present = {r[0] for r in db.execute(
                'SELECT book FROM delivery WHERE owner=? AND device=? AND present=1', (owner, device))}
            records = [['s', 'favorite', 'Избранное']]
            records.extend(['m', 'favorite', 'opds-' + r[0]] for r in db.execute(
                'SELECT book FROM favorite_books WHERE owner=? ORDER BY book', (owner,)) if r[0] in present)
            for c in db.execute('SELECT id,name FROM personal_collections WHERE owner=? ORDER BY name_key,id', (owner,)):
                records.append(['s', c['id'], c['name']])
                records.extend(['m', c['id'], 'opds-' + r[0]] for r in db.execute(
                    'SELECT book FROM personal_collection_books WHERE owner=? AND collection=? ORDER BY book',
                    (owner, c['id'])) if r[0] in present)
        if len(records) > 30000 or offset > len(records):
            raise WebError(409, 'Collection snapshot exceeds reader capacity')
        result = {'account': hashlib.sha256((salt + '\0' + owner).encode()).hexdigest(),
                  'revision': hashlib.sha256(encoded(records)).hexdigest(), 'records': [], 'next': 0}
        for i in range(offset, len(records)):
            result['records'].append(records[i])
            result['next'] = i + 1 if i + 1 < len(records) else 0
            if len(encoded(result)) > 1900:
                result['records'].pop()
                result['next'] = i
                break
        return result

    def _reader_collection_edit(self, owner, device, data):
        if not isinstance(data, dict):
            raise WebError(400, 'Expected collection operation')
        op, identity, action = (data.get(k, '') for k in ('op', 'id', 'action'))
        if (not isinstance(op, str) or not re.fullmatch('[a-f0-9]{32}', op)
                or not isinstance(identity, str) or not re.fullmatch('[a-zA-Z0-9_-]{1,64}', identity)
                or action not in ('create', 'member')):
            raise WebError(400, 'Invalid collection operation')
        fingerprint = hashlib.sha256(encoded(data)).hexdigest()
        # Check private-book ownership outside the write transaction (book() opens its own connection).
        if action == 'member':
            book = data.get('book', '')
            if not isinstance(book, str) or not re.fullmatch(r'opds-\d{1,26}', book) or type(data.get('selected')) is not bool:
                raise WebError(400, 'Invalid collection membership')
            book = book[5:]
            self.book(owner, book)
        with self.db() as db:
            db.execute('BEGIN IMMEDIATE')
            receipt = db.execute('SELECT identity,fingerprint FROM reader_collection_ops WHERE owner=? AND device=? AND op=?',
                                 (owner, device, op)).fetchone()
            if receipt:
                if receipt['fingerprint'] != fingerprint:
                    raise WebError(409, 'Operation identifier reused with different content')
                return {'id': receipt['identity']}
            if action == 'create':
                name = data.get('name')
                if not isinstance(name, str) or not name.strip() or len(name.strip()) > 80 or identity == 'favorite':
                    raise WebError(400, 'Invalid collection name')
                name = ' '.join(name.split())
                duplicate = db.execute('SELECT id FROM personal_collections WHERE owner=? AND name_key=?',
                                       (owner, normalized(name))).fetchone()
                existing = db.execute('SELECT id FROM personal_collections WHERE owner=? AND id=?', (owner, identity)).fetchone()
                if duplicate:
                    identity = duplicate['id']
                elif existing:
                    raise WebError(409, 'Collection identifier already exists')
                else:
                    if db.execute('SELECT count(*) FROM personal_collections WHERE owner=?', (owner,)).fetchone()[0] >= 100:
                        raise WebError(409, 'Collection capacity reached')
                    db.execute('INSERT INTO personal_collections VALUES (?,?,?,?,?)',
                               (owner, identity, name, normalized(name), time.time()))
            else:
                if identity != 'favorite':
                    self._collection(db, owner, identity)
                if data['selected']:
                    db.execute('INSERT OR IGNORE INTO personal_library VALUES (?,?,?)', (owner, book, time.time()))
                    if identity == 'favorite':
                        db.execute('INSERT OR IGNORE INTO favorite_books VALUES (?,?,?)', (owner, book, time.time()))
                    else:
                        db.execute('INSERT OR IGNORE INTO personal_collection_books VALUES (?,?,?)', (owner, identity, book))
                elif identity == 'favorite':
                    db.execute('DELETE FROM favorite_books WHERE owner=? AND book=?', (owner, book))
                else:
                    db.execute('DELETE FROM personal_collection_books WHERE owner=? AND collection=? AND book=?', (owner, identity, book))
            db.execute('INSERT INTO reader_collection_ops VALUES (?,?,?,?,?)', (owner, device, op, identity, fingerprint))
        return {'id': identity}
