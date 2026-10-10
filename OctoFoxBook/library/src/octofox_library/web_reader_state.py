"""Account-scoped, idempotent text-position and bookmark synchronization."""
import hashlib
import json
import re
import secrets
import time

from octofox_library.reader_anchors import AnchorMap
from octofox_library.web_errors import WebError


def wire(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'))


class ReaderStateMixin:
    def reader_state(self, session, device, book, digest, data=None, cursor=0):
        with self.read_gate:
            return self._reader_state(session, device, book, digest, data, cursor)

    def _reader_state(self, session, device, book, digest, data=None, cursor=0):
        if not re.fullmatch(r'[0-9]+', book) or not re.fullmatch(r'[a-f0-9]{64}', digest):
            raise WebError(400, 'Invalid book identity')
        self.book(session.owner, book)
        # Validate the actual reading copy: a replacement edition must never
        # receive positions from an older file that happens to have the same ID.
        payload, _ = self.download_book(session, book)
        if hashlib.sha256(payload).hexdigest() != digest:
            raise WebError(409, 'The book file has changed; reading state was preserved')
        from octofox_library.books_web import fb2_chapters
        mapping = AnchorMap(fb2_chapters(payload))
        owner = session.owner
        with self.db() as db:
            db.execute('BEGIN IMMEDIATE' if data is not None else 'BEGIN')
            salt = db.execute("SELECT value FROM reader_collection_meta WHERE key='identity'").fetchone()[0]
            account = hashlib.sha256((salt+'\0'+owner).encode()).hexdigest()

            def position():
                row = db.execute('SELECT * FROM reading WHERE owner=? AND book=?', (owner, book)).fetchone()
                if not row: return None, False
                if not row['anchor']:
                    if row['chapter'] or row['offset']:
                        raise WebError(409, 'Open this book on the website once to update its old reading position')
                    return None, row['shelf'] == 'read'
                return mapping.to_native(row['chapter'], json.loads(row['anchor'])), row['shelf'] == 'read'

            try:
                current, finished = position()
                if data is not None:
                    if data.get('account') != account or not re.fullmatch(r'[a-f0-9]{32}', str(data.get('op', ''))):
                        raise WebError(409, 'Device account changed; local state was preserved')
                    fingerprint = hashlib.sha256(wire(data).encode()).hexdigest()
                    previous = db.execute('SELECT fingerprint FROM reader_state_ops WHERE owner=? AND device=? AND op=?',
                                          (owner, device, data['op'])).fetchone()
                    if previous:
                        if previous[0] != fingerprint: raise WebError(409, 'Operation ID was reused')
                        return {'ok': True}
                    kind = data.get('kind')
                    if kind == 'position':
                        native = data.get('position')
                        if not isinstance(native, dict) or set(native) != {'record', 'byte'} or any(type(v) is not int for v in native.values()):
                            raise WebError(400, 'Invalid reading position')
                        if type(data.get('finished')) is not bool:
                            raise WebError(400, 'Invalid completion state')
                        chapter, anchor = mapping.to_web(native['record'], native['byte'])
                        # Explicit synchronization resumes the device's reading.
                        # If both sides advanced, retain the web position as a
                        # normal bookmark so neither place is lost.
                        if current and current != native and current != data.get('base'):
                            old_chapter, old_anchor = mapping.to_web(current['record'], current['byte'])
                            self._sync_bookmark(db, owner, book, secrets.token_hex(12), old_chapter,
                                                old_anchor, 'Previous position on another device')
                        db.execute('''INSERT INTO reading(owner,book,chapter,offset,shelf,updated,anchor) VALUES (?,?,?,0,?,?,?)
                            ON CONFLICT(owner,book) DO UPDATE SET chapter=excluded.chapter,offset=0,
                            shelf=excluded.shelf,updated=excluded.updated,anchor=excluded.anchor''',
                                   (owner, book, chapter, 'read' if data['finished'] else 'reading', time.time(), json.dumps(anchor)))
                        db.execute('INSERT OR REPLACE INTO reading_activity VALUES (?,?,?)', (owner, book, time.time()))
                    elif kind in ('bookmark', 'remove-bookmark'):
                        identity = data.get('id', '')
                        if not isinstance(identity, str) or not re.fullmatch(r'[a-f0-9]{24}', identity):
                            raise WebError(400, 'Invalid bookmark identity')
                        if kind == 'remove-bookmark':
                            db.execute('DELETE FROM reader_bookmarks WHERE owner=? AND book=? AND id=?', (owner, book, identity))
                        else:
                            native = data.get('position', {})
                            if not isinstance(native, dict) or any(type(native.get(k)) is not int for k in ('record', 'byte')):
                                raise WebError(400, 'Invalid bookmark position')
                            label = data.get('label', '')
                            if not isinstance(label, str) or not label.strip() or len(label) > 120:
                                raise WebError(400, 'Invalid bookmark label')
                            chapter, anchor = mapping.to_web(native['record'], native['byte'])
                            self._sync_bookmark(db, owner, book, identity, chapter, anchor, label)
                    else:
                        raise WebError(400, 'Unknown state operation')
                    db.execute('INSERT INTO reader_state_ops VALUES (?,?,?,?)', (owner, device, data['op'], fingerprint))
                    return {'ok': True}
                marks = []
                for row in db.execute('SELECT * FROM reader_bookmarks WHERE owner=? AND book=? ORDER BY chapter,block,char', (owner, book)):
                    native = mapping.to_native(row['chapter'], dict(block=row['block'], char=row['char']))
                    marks.append(dict(id=row['id'], position=native, label=row['label']))
                snapshot = dict(position=current, finished=finished, bookmarks=marks)
                if type(cursor) is not int or not 0 <= cursor <= len(marks): raise WebError(400, 'Invalid state cursor')
                result = dict(account=account, revision=hashlib.sha256(wire(snapshot).encode()).hexdigest(),
                              position=current, finished=finished, bookmarks=[], next=0)
                for index in range(cursor, len(marks)):
                    result['bookmarks'].append(marks[index]); result['next'] = index+1 if index+1 < len(marks) else 0
                    if len(wire(result).encode()) > 1900:
                        result['bookmarks'].pop(); result['next'] = index; break
                return result
            except (ValueError, UnicodeError) as error:
                raise WebError(409, 'This text position cannot be mapped safely; reading state was preserved') from error

    @staticmethod
    def _sync_bookmark(db, owner, book, identity, chapter, anchor, label):
        at = db.execute('SELECT id FROM reader_bookmarks WHERE owner=? AND book=? AND chapter=? AND block=? AND char=?',
                        (owner, book, chapter, anchor['block'], anchor['char'])).fetchone()
        if at:
            if at[0] == identity:
                db.execute('UPDATE reader_bookmarks SET label=? WHERE owner=? AND book=? AND id=?',
                           (label.strip(), owner, book, identity))
            return
        exists = db.execute('SELECT 1 FROM reader_bookmarks WHERE owner=? AND book=? AND id=?', (owner, book, identity)).fetchone()
        if not exists and db.execute('SELECT count(*) FROM reader_bookmarks WHERE owner=? AND book=?', (owner, book)).fetchone()[0] >= 500:
            raise WebError(409, 'Bookmark limit reached; local state was preserved')
        db.execute('''INSERT INTO reader_bookmarks VALUES (?,?,?,?,?,?,?,?,?) ON CONFLICT(owner,book,id)
            DO UPDATE SET chapter=excluded.chapter,block=excluded.block,char=excluded.char,label=excluded.label''',
                   (owner, book, identity, chapter, anchor['block'], anchor['char'], label.strip(), '', time.time()))
