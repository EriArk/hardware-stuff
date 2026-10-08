import json
import threading
from unittest.mock import patch

from test_books_web import BooksWebTest


class CatalogPerformanceTest(BooksWebTest):
    def test_unchanged_refresh_does_not_rebuild_facets(self):
        with self.app.db() as db:
            row = tuple(db.execute("SELECT * FROM books WHERE owner='alice'").fetchone())
        with patch.object(self.app, '_write_terms', wraps=self.app._write_terms) as terms:
            self.app._index_batch([row])
            terms.assert_not_called()
            changed = list(row)
            record = json.loads(row[7])
            record['author'] = 'Другой автор'
            record['authors'] = ['Другой автор']
            changed[4], changed[7] = record['author'], json.dumps(record, ensure_ascii=False)
            self.app._index_batch([tuple(changed)])
            terms.assert_called_once()
        self.assertEqual(self.app.book('alice', '42')['author'], 'Другой автор')

    def test_all_catalog_orderings_use_indexes_not_full_sort(self):
        for order in ['CAST(b.id AS INTEGER) DESC', 'normalized(b.title),CAST(b.id AS INTEGER)',
                      'normalized(b.title) DESC,CAST(b.id AS INTEGER)',
                      'normalized(b.author),normalized(b.title),b.id',
                      "normalized(b.series),CAST(json_extract(b.record,'$.seriesNumber') AS REAL),b.title,b.id"]:
            with self.app.db() as db:
                plan = db.execute('EXPLAIN QUERY PLAN SELECT b.record FROM books b WHERE b.owner=? ORDER BY '+order+' LIMIT 50', ('alice',)).fetchall()
            self.assertFalse(any('TEMP B-TREE' in r[3] for r in plan), (order, list(map(tuple, plan))))

    def test_refreshes_are_serialized_and_gate_released_after_failure(self):
        started, release = threading.Event(), threading.Event()
        with self.app.db() as db:
            db.execute('DELETE FROM catalog_state')
        def index(session):
            started.set()
            release.wait(3)
            self.app.indexing.discard(session.owner)
        with patch.object(self.app, '_index', side_effect=index) as indexed:
            self.app.ensure_index(self.session)
            self.assertTrue(started.wait(2))
            from octofox_library.books_web import Session
            other = Session('bob', 'Basic test', 'csrf', 9999999999)
            self.app.ensure_index(other)
            self.assertEqual(indexed.call_count, 1)
            self.assertNotIn('bob', self.app.indexing)
            release.set()
            self.assertTrue(self.app.index_gate.acquire(timeout=3))
            self.app.index_gate.release()
            with patch('octofox_library.books_web.threading.Thread.start', side_effect=RuntimeError('start')):
                with self.assertRaises(RuntimeError):
                    self.app.ensure_index(other)
            self.assertTrue(self.app.index_gate.acquire(blocking=False))
            self.app.index_gate.release()
