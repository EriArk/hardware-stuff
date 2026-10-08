"""Private, account-scoped shelves; independent of editorial collections and devices."""
import secrets
import time

from octofox_library.web_catalog import normalized
from octofox_library.web_errors import WebError


class PersonalCollectionsMixin:
    @staticmethod
    def _collection(db, owner, identity):
        if not isinstance(identity, str) or len(identity) > 64:
            raise WebError(400, 'Некорректная коллекция')
        row = db.execute('SELECT id,name FROM personal_collections WHERE owner=? AND id=?',
                         (owner, identity)).fetchone()
        if not row:
            raise WebError(404, 'Коллекция не найдена')
        return row

    def personal_collections(self, owner, data=None):
        changed = None
        with self.db() as db:
            if data is not None:
                if not isinstance(data, dict) or data.get('action') not in ('create', 'rename', 'delete'):
                    raise WebError(400, 'Некорректное действие с коллекцией')
                action = data['action']
                name = data.get('name', '')
                if action != 'delete':
                    if not isinstance(name, str) or not name.strip() or len(name.strip()) > 80:
                        raise WebError(400, 'Название: от 1 до 80 символов')
                    name = ' '.join(name.split())
                db.execute('BEGIN IMMEDIATE')
                identity = data.get('id')
                if action != 'create':
                    self._collection(db, owner, identity)
                if action != 'delete':
                    duplicate = db.execute('SELECT id FROM personal_collections WHERE owner=? AND name_key=?',
                                           (owner, normalized(name))).fetchone()
                    if duplicate and action == 'create':
                        # Retrying an ambiguous network response must not create a duplicate.
                        identity = duplicate['id']
                    elif duplicate and duplicate['id'] != identity:
                        raise WebError(409, 'Коллекция с таким названием уже есть')
                    elif action == 'create':
                        if db.execute('SELECT count(*) FROM personal_collections WHERE owner=?', (owner,)).fetchone()[0] >= 100:
                            raise WebError(400, 'Можно создать до 100 коллекций')
                        identity = secrets.token_hex(12)
                        db.execute('INSERT INTO personal_collections VALUES (?,?,?,?,?)',
                                   (owner, identity, name, normalized(name), time.time()))
                    else:
                        db.execute('UPDATE personal_collections SET name=?,name_key=? WHERE owner=? AND id=?',
                                   (name, normalized(name), owner, identity))
                else:
                    db.execute('DELETE FROM personal_collection_books WHERE owner=? AND collection=?', (owner, identity))
                    db.execute('DELETE FROM personal_collections WHERE owner=? AND id=?', (owner, identity))
                changed = identity
            rows = db.execute('''SELECT c.id,c.name,count(p.book) AS count FROM personal_collections c
                LEFT JOIN personal_collection_books m ON m.owner=c.owner AND m.collection=c.id
                LEFT JOIN personal_library p ON p.owner=m.owner AND p.book=m.book
                WHERE c.owner=? GROUP BY c.id ORDER BY c.name_key,c.id''', (owner,)).fetchall()
        return {'collections': [dict(r) for r in rows], 'id': changed}

    def book_collections(self, owner, book_id, data=None):
        self.book(owner, book_id)  # Do not allow a private upload from another account.
        with self.db() as db:
            if data is not None:
                if not isinstance(data, dict) or type(data.get('selected')) is not bool:
                    raise WebError(400, 'Ожидалось состояние выбора коллекции')
                db.execute('BEGIN IMMEDIATE')
                identity = data.get('id')
                self._collection(db, owner, identity)
                if data['selected']:
                    db.execute('INSERT OR IGNORE INTO personal_library VALUES (?,?,?)', (owner, book_id, time.time()))
                    db.execute('INSERT OR IGNORE INTO personal_collection_books VALUES (?,?,?)', (owner, identity, book_id))
                else:
                    db.execute('DELETE FROM personal_collection_books WHERE owner=? AND collection=? AND book=?',
                               (owner, identity, book_id))
            rows = db.execute('''SELECT c.id,c.name,EXISTS(SELECT 1 FROM personal_collection_books m
                WHERE m.owner=c.owner AND m.collection=c.id AND m.book=?) AS selected
                FROM personal_collections c WHERE c.owner=? ORDER BY c.name_key,c.id''', (book_id, owner)).fetchall()
            saved = db.execute('SELECT 1 FROM personal_library WHERE owner=? AND book=?', (owner, book_id)).fetchone()
        return {'collections': [dict(r) | {'selected': bool(r['selected'])} for r in rows], 'inLibrary': bool(saved)}
