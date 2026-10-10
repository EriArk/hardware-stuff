import base64
import io
import json
import tempfile
import threading
import time
import unittest
import xml.etree.ElementTree as ET
from email.message import Message
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from octofox_library.books_web import (
    LibraryWeb,
    Session,
    WebError,
    WebServer,
    entry_record,
    fb2_chapters,
)
from octofox_library.opds_facade import UpstreamFailure
from octofox_library.web_catalog import book_terms


class BooksWebTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "web.sqlite3"
        self.app = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        self.session = Session("alice", "Basic test", "csrf-test", time.time() + 1000)
        self.seed("alice")
        self.seed("bob")

    def seed(self, owner):
        book = {
            "id": "42",
            "title": "Тени солнца",
            "author": "Автор",
            "authors": ["Автор"],
            "series": "Серия",
            "seriesNumber": "2",
            "summary": "Аннотация",
            "updated": "2026-09-08",
            "genres": ["sf_heroic"],
            "language": "ru",
            "cover": "/reader-api/books/42/cover",
        }
        self.app._index_batch(
            [
                (
                    owner,
                    "42",
                    book["title"].casefold(),
                    "тени солнца автор серия",
                    "Автор",
                    "Серия",
                    book["updated"],
                    json.dumps(book),
                )
            ]
        )
        with self.app.db() as db:
            db.execute("INSERT INTO catalog_state VALUES (?,?)", (owner, time.time()))

    def test_catalog_fifty_per_page_no_duplicates_or_skips(self):
        for i in range(43, 144):
            self.add_book(str(i))
        pages = [self.app.catalog(self.session, {"page": [str(i)]}) for i in range(1, 4)]
        self.assertEqual([len(p["books"]) for p in pages], [50, 50, 2])
        self.assertEqual([p["more"] for p in pages], [True, True, False])
        self.assertEqual(len({b["id"] for p in pages for b in p["books"]}), 102)

    def test_collections_membership_filters_accounts_and_new_imports(self):
        query = {"collection": ["korean-elves"]}
        self.add_book("43", title="Кошка", genres=["child_sf_fantasy", "корейские новеллы"])
        self.add_book("44", title="Дорама", series="Лучшие дорамы", genres=["love_history"])
        self.add_book("45", title="Русская история", authors=["Анатолий Ким"], genres=["prose"])
        self.add_book("46", title="История Азии", genres=["sci_history", "Азия"])
        self.add_book("47", title="Личная история", genres=["корейская литература"])
        with self.app.db() as db:
            db.execute("INSERT INTO uploaded_books VALUES (?,?,?,?,?)", ("alice", "47", "digest", 100, 1))
        result = self.app.catalog(self.session, query)
        self.assertEqual({b["id"] for b in result["books"]}, {"43", "44"})
        self.assertEqual(self.app.collections(self.session)["collections"][0]["count"], 2)
        bob = Session("bob", "Basic test", "csrf", time.time() + 1000)
        self.assertEqual(self.app.collections(bob)["collections"][0]["count"], 0)
        self.assertEqual(self.app.catalog(bob, query)["total"], 0)
        self.assertEqual(self.app.catalog(self.session, query | {"q": ["кош"]})["total"], 1)
        self.assertEqual(self.app.catalog(self.session, query | {"genre": ["love_history"]})["total"], 1)
        self.add_book("48", title="Новая история", genres=["корейское фэнтези"])
        self.assertEqual(self.app.catalog(self.session, query)["total"], 3)
        # Changing metadata removes a book too; no frozen IDs or copied files.
        self.add_book("43", genres=["prose"])
        self.assertEqual(self.app.collections(self.session)["collections"][0]["count"], 2)
        with self.assertRaises(WebError) as error:
            self.app.catalog(self.session, {"collection": ["' OR 1=1 --"]})
        self.assertEqual(error.exception.status, 400)

    def test_collections_use_normal_catalog_pagination(self):
        for i in range(100, 205):
            self.add_book(str(i), genres=["корейские новеллы"])
        pages = [self.app.catalog(self.session, {"collection": ["korean-elves"], "page": [str(p)]}) for p in (1, 2, 3)]
        self.assertEqual([len(p["books"]) for p in pages], [50, 50, 5])
        self.assertEqual(len({b["id"] for p in pages for b in p["books"]}), 105)

    def test_catalog_russian_filters_and_account_isolation(self):
        result = self.app.catalog(self.session, {"q": ["ТЕНИ"], "genre": ["sf_heroic"]})
        self.assertEqual(result["total"], 1)
        self.assertEqual(self.app.catalog(self.session, {"q": ["%"]})["total"], 0)
        self.assertEqual(self.app.catalog(self.session, {"series": ["нет"]})["total"], 0)
        self.assertNotEqual(self.app.facets("alice")["items"][0]["label"], "sf_heroic")
        with self.assertRaises(WebError):
            self.app.book("nobody", "42")

    def add_book(self, book_id="43", **changes):
        record = self.app.book("alice", "42") | {"id": book_id} | changes
        self.app._index_batch(
            [
                (
                    "alice",
                    book_id,
                    record["title"].casefold(),
                    "",
                    record["author"],
                    record["series"],
                    record["updated"],
                    json.dumps(record, ensure_ascii=False),
                )
            ]
        )

    def test_new_arrivals_are_last_100_before_filters_and_pagination(self):
        for number in range(43, 151):
            self.add_book(str(number), title=f"Book {number}")
        ids = []
        for page in range(1, 3):
            result = self.app.catalog(self.session, {"new": ["1"], "page": [str(page)], "sort": ["title"]})
            self.assertEqual(result["total"], 100)
            self.assertEqual(result["more"], page < 2)
            ids.extend(b["id"] for b in result["books"])
        self.assertEqual(ids, [str(i) for i in range(150, 50, -1)])
        self.assertEqual(self.app.catalog(self.session, {"new": ["1"], "page": ["3"]})["books"], [])
        self.add_book("43", title="OutsideArrivals")
        self.assertEqual(self.app.catalog(self.session, {"new": ["1"], "q": ["OutsideArrivals"]})["total"], 0)
        self.assertEqual(self.app.catalog(self.session, {})["total"], 109)
        # Updating old metadata must not turn it into an arrival.
        self.add_book("42", updated="2099-01-01", title="Edited")
        self.assertEqual(self.app.catalog(self.session, {"new": ["1"]})["books"][0]["id"], "150")
        # A new indexed book appears without a process restart.
        self.add_book("151")
        self.assertEqual(self.app.catalog(self.session, {"new": ["1"]})["books"][0]["id"], "151")
        bob = Session("bob", "Basic other", "csrf-other", time.time() + 1000)
        self.assertEqual(self.app.catalog(bob, {"new": ["1"]})["total"], 1)

    def test_catalog_status_automatically_starts_expired_index_only_once(self):
        with patch("octofox_library.books_web.threading.Thread") as thread:
            self.app.catalog_status(self.session)
            thread.assert_not_called()
            with self.app.db() as db:
                db.execute("UPDATE catalog_state SET refreshed=1 WHERE owner='alice'")
            status = self.app.catalog_status(self.session)
            self.assertEqual(status, {"revision": 1, "status": "loading"})
            self.app.catalog_status(self.session)
            thread.assert_called_once()
            thread.return_value.start.assert_called_once()

    def test_text_anchor_is_durable_account_scoped_and_backward_compatible(self):
        anchor = {"block": 7, "char": 25}
        self.app.save_state("alice", "42", {"chapter": 2, "offset": 0.4, "anchor": anchor})
        self.app.favorite_book("alice", "42", True)
        self.app.save_state("alice", "42", {"shelf": "read"})
        reopened = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        self.assertEqual(reopened.book("alice", "42")["reading"]["anchor"], anchor)
        self.assertNotIn("anchor", reopened.book("bob", "42")["reading"])
        for value in ({"block": -1, "char": 2}, {"block": 1, "char": True}, [], {"block": 2}, {"block": 1, "char": 3, "extra": 0}):
            with self.assertRaises(WebError):
                self.app.save_state("alice", "42", {"chapter": 2, "anchor": value})
        with self.assertRaises(WebError):
            self.app.save_state("alice", "42", {"anchor": anchor})
        self.app.save_state("alice", "42", {"chapter": 3, "offset": 0.1})
        self.assertNotIn("anchor", self.app.book("alice", "42")["reading"])
        self.assertTrue(self.app.book("alice", "42")["isFavorite"])

    def test_real_genres_separate_from_tags_and_aliases_deduplicated(self):
        self.add_book(
            genres=["sf_fantasy_city", "city_fantasy", "магические миры", "фэнтези про драконов"]
        )
        terms = book_terms(self.app.book("alice", "43"))
        genres = [t for t in terms if t[0] == "genre"]
        tags = [t for t in terms if t[0] == "tag"]
        self.assertEqual(len(genres), 1)
        self.assertEqual(genres[0][1], "city_fantasy")
        self.assertEqual(len(tags), 2)
        self.assertEqual({t[4] for t in tags}, {"magic", "style"})
        result = self.app.catalog(self.session, {"genreGroup": ["fantasy"]})
        self.assertEqual(result["total"], 1)
        self.assertEqual(result["books"][0]["genreItems"][0]["label"], "Городское фэнтези")
        self.assertEqual(self.app.catalog(self.session, {"genre": ["магические миры"]})["total"], 0)
        self.assertEqual(self.app.catalog(self.session, {"tag": ["магические миры"]})["total"], 1)
        self.assertEqual(self.app.catalog(self.session, {"tagGroup": ["magic"]})["total"], 1)

    def test_search_scopes_partial_names_yo_multiple_words_and_coauthors(self):
        self.add_book(
            author="Пётр Иванов, Анна Соколова",
            authors=["Пётр Иванов", "Анна Соколова"],
            series="Звёздный путь",
            genres=["sf_space", "драконы"],
        )
        for q, scope in [
            ("петр ив", "author"),
            ("звезд путь", "series"),
            ("космическая", "genre"),
            ("дракон", "tag"),
        ]:
            self.assertEqual(
                self.app.catalog(self.session, {"q": [q], "scope": [scope]})["total"], 1
            )
        self.assertEqual(
            self.app.catalog(self.session, {"q": ["дракон"], "scope": ["genre"]})["total"], 0
        )
        self.assertEqual(
            self.app.catalog(self.session, {"q": ["петр"], "scope": ["title"]})["total"], 0
        )
        self.assertEqual(self.app.catalog(self.session, {"author": ["Анна Соколова"]})["total"], 1)
        self.assertEqual(
            self.app.catalog(self.session, {"author": ["Анна Соколова"], "genre": ["sf_space"]})[
                "total"
            ],
            1,
        )
        self.assertEqual(
            self.app.catalog(self.session, {"author": ["Анна Соколова"], "genre": ["sf_heroic"]})[
                "total"
            ],
            0,
        )
        result = self.app.facets("alice", {"kind": ["author"], "q": ["петр"]})
        self.assertEqual(result["items"][0]["label"], "Пётр Иванов")

    def test_directories_pagination_alphabet_and_owner_isolation(self):
        for n in range(46):
            self.add_book(
                str(100 + n),
                title=f"Книга {n:02}",
                author=f"Борис {n:02}",
                authors=[f"Борис {n:02}"],
                series=f"Серия {n:02}",
            )
        first = self.app.facets("alice", {"kind": ["author"], "letter": ["Б"]})
        second = self.app.facets("alice", {"kind": ["author"], "letter": ["Б"], "page": ["2"]})
        self.assertEqual((first["total"], len(first["items"]), len(second["items"])), (46, 40, 6))
        self.assertTrue(first["more"])
        self.assertFalse(second["more"])
        self.assertFalse(
            {x["value"] for x in first["items"]} & {x["value"] for x in second["items"]}
        )
        self.assertIn("Б", first["letters"])
        self.assertEqual(self.app.facets("bob", {"kind": ["author"]})["total"], 1)
        self.assertEqual(
            self.app.facets("alice", {"kind": ["tag"], "q": ["%' OR 1=1"]})["total"], 0
        )
        with self.assertRaises(WebError):
            self.app.facets("alice", {"kind": ["not_a_column"]})

    def test_migration_preserves_queue_and_positions(self):
        self.add_book(genres=["sf_fantasy", "магия"])
        self.app.enqueue("alice", "42", "default")
        with self.app.db() as db:
            db.execute(
                "INSERT INTO reading(owner,book,chapter,offset,shelf,updated) VALUES (?,?,?,?,?,?)",
                ("alice", "42", 3, 0.2, "reading", time.time()),
            )
            db.execute("DELETE FROM book_facets")
            db.execute("DELETE FROM web_schema")
        restarted = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        self.assertEqual(restarted.facets("alice", {"kind": ["tag"]})["total"], 1)
        self.assertEqual(restarted.book("alice", "42")["reading"]["chapter"], 3)
        self.assertEqual(len(restarted.deliveries("alice")), 1)

    def test_group_counts_count_books_not_tags_and_update_removes_old_terms(self):
        self.add_book(genres=["sf_fantasy", "city_fantasy", "драконы", "ведьмы"])
        groups = self.app.facets("alice")["groups"]
        self.assertEqual(next(g["count"] for g in groups if g["value"] == "fantasy"), 1)
        self.add_book(genres=["sf_space"])
        self.assertEqual(self.app.facets("alice", {"kind": ["tag"]})["total"], 0)
        self.assertEqual(self.app.facets("alice", {"group": ["fantasy"]})["total"], 0)

    def test_title_sort_both_directions(self):
        self.add_book(title="Альфа")
        self.add_book("44", title="Янтарь")
        result = self.app.catalog(self.session, {"sort": ["title"]})
        self.assertEqual([b["title"] for b in result["books"]], ["Альфа", "Тени солнца", "Янтарь"])
        result = self.app.catalog(self.session, {"sort": ["title_desc"]})
        self.assertEqual(result["books"][0]["title"], "Янтарь")

    def test_booklore_atom_collection_and_calibre_series(self):
        entry = ET.fromstring("""<entry xmlns="http://www.w3.org/2005/Atom">
          <title>Book</title><meta property="belongs-to-collection" id="series">Cycle</meta>
          <meta property="group-position" refines="#other">99</meta>
          <meta property="group-position" refines="#series">2.5</meta>
          <link rel="http://opds-spec.org/acquisition" href="/api/v1/opds/42/download?fileId=42"/>
          </entry>""")
        self.assertEqual(entry_record(entry)["series"], "Cycle")
        self.assertEqual(entry_record(entry)["seriesNumber"], "2.5")
        from octofox_library.opds_facade import CALIBRE

        legacy = ET.fromstring(f'''<entry xmlns="http://www.w3.org/2005/Atom" xmlns:c="{CALIBRE}">
          <title>Book</title><c:series>Old cycle</c:series><c:series_index>3</c:series_index>
          <link rel="http://opds-spec.org/acquisition" href="/api/v1/opds/42/download"/></entry>''')
        self.assertEqual(entry_record(legacy)["series"], "Old cycle")
        self.assertEqual(entry_record(legacy)["seriesNumber"], "3")

    def test_numeric_series_order(self):
        self.add_book("43", seriesNumber="10")
        self.add_book("44", seriesNumber="1")
        result = self.app.catalog(self.session, {"series": ["Серия"], "sort": ["series"]})
        self.assertEqual([b["seriesNumber"] for b in result["books"]], ["1", "2", "10"])

    def test_enqueue_is_idempotent(self):
        first = self.app.enqueue("alice", "42", "default")
        second = self.app.enqueue("alice", "42", "default")
        self.assertEqual(first["id"], second["id"])
        self.assertEqual(len(self.app.deliveries("alice")), 1)
        self.assertEqual(self.app.deliveries("bob"), [])

    def test_personal_library_is_independent_persistent_and_idempotent(self):
        self.app.enqueue("alice", "42", "default")
        with self.app.db() as db:
            db.execute(
                "INSERT INTO reading(owner,book,chapter,offset,shelf,updated) VALUES (?,?,?,?,?,?)",
                ("alice", "42", 2, 0.4, "favorite", time.time()),
            )
        self.app.save_book("alice", "42", True)
        self.app.save_book("alice", "42", True)
        self.assertTrue(self.app.book("alice", "42")["inLibrary"])
        self.assertFalse(self.app.book("bob", "42")["inLibrary"])
        self.assertEqual(self.app.catalog(self.session, {"personal": ["1"]})["total"], 1)
        self.assertEqual(
            self.app.catalog(self.session, {"personal": ["1"], "q": ["no-match"]})["total"], 0
        )
        restarted = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        self.assertTrue(restarted.book("alice", "42")["inLibrary"])
        restarted.save_book("alice", "42", False)
        self.assertFalse(self.app.book("alice", "42")["inLibrary"])
        self.assertEqual(self.app.book("alice", "42")["reading"]["shelf"], "favorite")
        self.assertEqual(self.app.book("alice", "42")["reading"]["chapter"], 2)
        self.assertEqual(len(self.app.deliveries("alice")), 1)
        self.assertEqual(self.app.catalog(self.session, {})["total"], 1)
        self.assertEqual(self.app.catalog(self.session, {"personal": ["1"]})["total"], 0)
        with self.assertRaises(WebError):
            self.app.save_book("alice", "42", "false")
        with self.assertRaises(WebError):
            self.app.save_book("nobody", "42", True)

    def test_personal_library_newest_saved_order(self):
        self.add_book("43")
        with patch("octofox_library.books_web.time.time", return_value=100):
            self.app.save_book("alice", "43", True)
        with patch("octofox_library.books_web.time.time", return_value=200):
            self.app.save_book("alice", "42", True)
        result = self.app.catalog(self.session, {"personal": ["1"], "sort": ["recent"]})
        self.assertEqual([b["id"] for b in result["books"]], ["42", "43"])

    def test_reading_position_favorite_and_library_are_independent(self):
        self.app.save_book("alice", "42", True)
        self.app.save_state("alice", "42", {"chapter": 4, "offset": 0.65, "shelf": "reading"})
        self.app.favorite_book("alice", "42", True)
        self.app.save_state("alice", "42", {"shelf": "read"})
        self.assertEqual(
            self.app.book("alice", "42")["reading"], {"chapter": 4, "offset": 0.65, "shelf": "read"}
        )
        self.app.save_state("alice", "42", {"shelf": "reading"})
        self.app.save_book("alice", "42", False)
        self.app.save_book("alice", "42", True)
        reopened = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        book = reopened.book("alice", "42")
        self.assertEqual(book["reading"]["chapter"], 4)
        self.assertEqual(book["reading"]["offset"], 0.65)
        self.assertTrue(book["isFavorite"])
        self.assertTrue(book["inLibrary"])
        self.assertEqual(reopened.catalog(self.session, {"shelf": ["reading"]})["total"], 1)
        self.assertEqual(reopened.catalog(self.session, {"shelf": ["favorite"]})["total"], 1)
        self.assertFalse(reopened.book("bob", "42")["isFavorite"])
        reopened.favorite_book("alice", "42", False)
        self.assertEqual(reopened.catalog(self.session, {"shelf": ["favorite"]})["total"], 0)
        self.assertEqual(reopened.book("alice", "42")["reading"], book["reading"])
        with self.assertRaises(WebError):
            reopened.favorite_book("alice", "42", "false")
        with self.assertRaises(WebError):
            reopened.favorite_book("nobody", "42", True)

    def test_legacy_favorite_migrates_once_without_losing_position(self):
        with self.app.db() as db:
            db.execute("DELETE FROM web_schema WHERE version=3")
            db.execute(
                "INSERT INTO reading(owner,book,chapter,offset,shelf,updated) VALUES (?,?,?,?,?,?)", ("alice", "42", 2, 0.3, "favorite", 123)
            )
        restarted = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        self.assertTrue(restarted.book("alice", "42")["isFavorite"])
        self.assertEqual(restarted.catalog(self.session, {"shelf": ["reading"]})["total"], 1)
        restarted.favorite_book("alice", "42", False)
        again = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        self.assertFalse(again.book("alice", "42")["isFavorite"])
        self.assertEqual(again.book("alice", "42")["reading"]["offset"], 0.3)

    def test_reading_now_sort_and_catalog_position(self):
        self.add_book("43")
        with patch("octofox_library.books_web.time.time", return_value=100):
            self.app.save_state("alice", "43", {"shelf": "reading", "chapter": 1})
        with patch("octofox_library.books_web.time.time", return_value=200):
            self.app.save_state("alice", "42", {"shelf": "reading", "chapter": 3, "offset": 0.2})
        result = self.app.catalog(self.session, {"shelf": ["reading"], "sort": ["recent"]})
        self.assertEqual([b["id"] for b in result["books"]], ["42", "43"])
        self.assertEqual(result["books"][0]["reading"]["offset"], 0.2)
        # Finishing/marking a book favorite is not a reading event.
        self.app.save_state("alice", "43", {"shelf": "read"})
        self.app.favorite_book("alice", "43", True)
        self.app.save_book("alice", "43", True)
        result = self.app.catalog(self.session, {"shelf": ["reading"], "sort": ["title"]})
        self.assertEqual([b["id"] for b in result["books"]], ["42", "43"])
        self.app.save_state("alice", "43", {"chapter": 2, "offset": 0.1})
        result = self.app.catalog(self.session, {"shelf": ["reading"]})
        self.assertEqual([b["id"] for b in result["books"]], ["43", "42"])
        self.assertEqual(
            self.app.catalog(Session("bob", "", "", 0), {"shelf": ["reading"]})["total"], 0
        )
        self.add_book("44")
        self.app.save_state("alice", "44", {"shelf": "reading"})
        self.assertEqual(self.app.catalog(self.session, {"shelf": ["reading"]})["total"], 2)

    @staticmethod
    def file_response(data=b"<FictionBook/>", mime="application/x-fictionbook+xml"):
        response = io.BytesIO(data)
        response.headers = Message()
        response.headers["Content-Type"] = mime
        return response

    def test_download_preserves_bytes_safe_filename_and_account_checks(self):
        self.add_book(title='Тест/книга\r\n"wrong"')
        payload = "<FictionBook>Книга</FictionBook>".encode()
        with patch.object(
            self.app.client, "open", return_value=self.file_response(payload)
        ) as upstream:
            data, name = self.app.download_book(self.session, "43")
            upstream.assert_called_once_with("/43/download", (), self.session.authorization)
        self.assertEqual(data, payload)
        self.assertIn("attachment;", name)
        self.assertIn("filename*=UTF-8''%D0", name)
        self.assertNotIn("\r", name)
        self.assertNotIn("\n", name)
        self.assertNotIn("%2F", name)
        with patch.object(self.app.client, "open") as upstream:
            with self.assertRaises(WebError):
                self.app.download_book(Session("other", "", "", 0), "42")
            upstream.assert_not_called()
        for payload, mime in [(b"", "application/xml"), (b"<html/>", "text/html")]:
            with patch.object(
                self.app.client, "open", return_value=self.file_response(payload, mime)
            ):
                with self.assertRaises(WebError):
                    self.app.download_book(self.session, "42")
        with (
            patch("octofox_library.books_web.MAX_BOOK", 5),
            patch.object(self.app.client, "open", return_value=self.file_response(b"123456")),
        ):
            with self.assertRaises(WebError):
                self.app.download_book(self.session, "42")

    def test_first_device_adopts_waiting_books_only_for_owner(self):
        self.app.enqueue("alice", "42", "default")
        self.app.enqueue("bob", "42", "default")
        wire = self.app.poll("alice", "reader-aabbcc")
        self.assertIn(b"\topds-42\t", wire)
        self.assertEqual(self.app.deliveries("alice")[0]["state"], "downloading")
        self.assertEqual(self.app.deliveries("bob")[0]["device"], "default")
        self.assertEqual(self.app.poll("alice", "reader-ddeeff"), b"")
        self.assertEqual(self.app.poll("alice", "reader-aabbcc"), b"")

    def test_device_delivery_uses_configured_request_address_not_install_address(self):
        public = "https://reader.example"
        self.app.enqueue("alice", "42", "default")
        with patch.object(self.app.network, "allowed", return_value=[self.app.origin, public]):
            with self.assertRaises(WebError):
                self.app.poll("alice", "reader-aabbcc", origin="https://unconfigured.example")
            self.assertEqual(self.app.deliveries("alice")[0]["state"], "queued")
            wire = self.app.poll("alice", "reader-aabbcc", origin=public).decode().strip().split("\t")
        self.assertTrue(wire[3].startswith(public + "/reader-api/"))
        self.assertTrue(wire[4].startswith(public + "/reader-api/"))

    def test_receipt_is_scoped_and_requires_verified_digest(self):
        job = self.app.enqueue("alice", "42", "default")["id"]
        self.app.poll("alice", "reader-aabbcc")
        for owner, device, digest in [
            ("bob", "reader-aabbcc", "a" * 64),
            ("alice", "reader-other", "a" * 64),
            ("alice", "reader-aabbcc", ""),
        ]:
            with self.assertRaises(WebError):
                self.app.acknowledge(owner, device, job, "delivered", digest)
        self.app.acknowledge("alice", "reader-aabbcc", job, "delivered", "a" * 64)
        self.app.acknowledge("alice", "reader-aabbcc", job, "delivered", "a" * 64)
        self.assertEqual(self.app.poll("alice", "reader-aabbcc"), b"")

    def test_queue_survives_restart_and_expired_lease(self):
        self.app.enqueue("alice", "42", "default")
        self.app.poll("alice", "reader-aabbcc")
        with self.app.db() as db:
            db.execute("UPDATE delivery SET updated=0")
        restarted = LibraryWeb(self.path, "http://127.0.0.1:1/api/v1/opds", "http://127.0.0.1")
        self.assertIn(b"opds-42", restarted.poll("alice", "reader-aabbcc"))

    def test_simultaneous_polls_lease_only_once(self):
        self.app.enqueue("alice", "42", "default")
        results = []
        threads = [
            threading.Thread(target=lambda: results.append(self.app.poll("alice", "reader-aabbcc")))
            for _ in range(4)
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(sum(bool(value) for value in results), 1)

    def test_invalid_device_and_foreign_target_rejected(self):
        with self.assertRaises(WebError):
            self.app.poll("alice", "../somewhere")
        with self.assertRaises(WebError):
            self.app.enqueue("alice", "42", "reader-bob")

    def test_login_keeps_existing_identity_and_library(self):
        self.app.save_state("alice", "42", {"chapter": 2, "offset": 0.4,
                                               "anchor": {"block": 7, "char": 25}})
        self.app.favorite_book("alice", "42", True)
        before = self.app.book("alice", "42")
        expected_auth = "Basic " + base64.b64encode(b"alice:private-password").decode()
        with patch.object(self.app.client, "fetch_xml", return_value=None) as upstream, \
                patch.object(self.app, "ensure_index") as index, \
                patch.object(self.app.speech, "warm_library") as warm:
            for username in ("alice", "alice"):
                with self.subTest(username=username):
                    token, session = self.app.login(username, "private-password")
                    self.assertIs(self.app.sessions[token], session)
                    self.assertEqual(session.owner, "alice")
                    self.assertEqual(session.authorization, expected_auth)
                    self.assertEqual(self.app.book(session.owner, "42"), before)
                    index.assert_called_with(session)
                    warm.assert_called_with(session)
            upstream.assert_called_once_with("", (), expected_auth)
        self.assertNotIn(b"private-password", self.path.read_bytes())

    def test_login_does_not_bypass_password_check_or_issue_session(self):
        with patch.object(self.app.client, "fetch_xml", return_value=None), \
                patch.object(self.app, "ensure_index"), \
                patch.object(self.app.speech, "warm_library"):
            self.app.login("alice-books", "right-password")
        before = dict(self.app.sessions)
        with patch.object(self.app.client, "fetch_xml", side_effect=UpstreamFailure(401)) as upstream:
            with self.assertRaises(UpstreamFailure):
                self.app.login("alice-books", "wrong-password")
            expected_auth = "Basic " + base64.b64encode(b"alice-books:wrong-password").decode()
            upstream.assert_called_once_with("", (), expected_auth)
        self.assertEqual(dict(self.app.sessions), before)

    def test_logins_are_not_rewritten(self):
        with patch.object(self.app.client, "fetch_xml", return_value=None) as upstream, \
                patch.object(self.app, "ensure_index"), \
                patch.object(self.app.speech, "warm_library"):
            for username in ("carol", "alice", "alice-books", "alice-books-books"):
                with self.subTest(username=username):
                    _, session = self.app.login(username, "test-password")
                    self.assertEqual(session.owner, username)
                    expected = "Basic " + base64.b64encode(f"{username}:test-password".encode()).decode()
                    upstream.assert_called_with("", (), expected)
            upstream.reset_mock()
            for username, password in (("alice-books:evil", "x"), ("alice-books", "x" * 513),
                                       ("alice-books", ""), ("", "x")):
                with self.assertRaises(WebError) as error:
                    self.app.login(username, password)
                self.assertEqual(error.exception.status, 401)
            upstream.assert_not_called()

    def test_credentials_verified_and_never_persisted(self):
        auth = "Basic " + base64.b64encode(b"alice:a-private-password").decode()
        with patch.object(self.app.client, "fetch_xml", return_value=None) as upstream:
            self.assertEqual(self.app.authenticate(auth), "alice")
            upstream.assert_called_once()
        with patch.object(self.app.client, "fetch_xml", side_effect=UpstreamFailure(401)):
            with self.assertRaises(UpstreamFailure):
                self.app.authenticate("Basic " + base64.b64encode(b"alice:wrong").decode())
        self.assertNotIn(b"a-private-password", self.path.read_bytes())

    def test_fb2_is_safe_text_with_formatting(self):
        payload = (
            b'<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">'
            b"<body><section><title><p>Chapter</p></title>"
            b"<p>A <strong>bold</strong> &lt;script&gt;alert(1)&lt;/script&gt;"
            b'<a href="javascript:alert(1)">link</a></p>'
            b'<image href="https://evil.test/x"/></section></body></FictionBook>'
        )
        result = fb2_chapters(payload)
        self.assertIn("<strong>bold</strong>", result[0]["html"])
        self.assertNotIn("<script>", result[0]["html"])
        self.assertNotIn("javascript:", result[0]["html"])
        self.assertNotIn("evil.test", result[0]["html"])
        with self.assertRaises(WebError):
            fb2_chapters(b'<!DOCTYPE x [<!ENTITY a "oops">]><FictionBook/>')

    def test_pagination_batches_are_bounded_authenticated_and_read_only(self):
        auth = patch.object(self.app, "authenticate", return_value="alice")
        auth.start()
        self.addCleanup(auth.stop)
        server = WebServer(("127.0.0.1", 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.app.sessions["test-token"] = self.session
        base = f"http://127.0.0.1:{server.server_port}/reader-api/books/"
        cookie = {"Cookie": "books_session=test-token"}
        chapters = [{"title": str(n), "html": f"<p>{n}</p>"} for n in range(40)]
        before = self.app.book("alice", "42")["reading"]
        def read(query, book="42", headers=cookie):
            with urlopen(Request(base + book + "/read?" + query, headers=headers), timeout=5) as response:
                return json.load(response)
        with patch.object(self.app, "chapters", return_value=chapters) as parsed:
            with self.assertRaises(HTTPError) as error:
                read("start=0", headers={})
            self.assertEqual(error.exception.code, 401)
            with self.assertRaises(HTTPError) as error:
                read("start=0", book="999")
            self.assertEqual(error.exception.code, 404)
            parsed.assert_not_called()
            self.assertEqual(read("chapter=3")["html"], chapters[3]["html"])
            result = read("start=0&limit=999")
            self.assertEqual((result["total"], result["next"], len(result["items"])), (40, 32, 32))
            tail = read("start=32&limit=32")
            self.assertEqual([v["chapter"] for v in tail["items"]], list(range(32, 40)))
            self.assertEqual(read("start=999")["items"], [])
            chapters[:] = [{"title": str(n), "html": "я" * 70000} for n in range(5)]
            self.assertEqual(read("start=0&limit=32")["next"], 1, 'Bound UTF-8 bytes, not characters')
        self.assertEqual(self.app.book("alice", "42")["reading"], before)

    def test_http_auth_csrf_origin_and_saved_position(self):
        server = WebServer(("127.0.0.1", 0), self.app)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        base = f"http://127.0.0.1:{server.server_port}"
        self.app.origin = base
        self.app.sessions["test-token"] = self.session
        auth = patch.object(self.app, "authenticate", return_value="alice")
        auth.start()
        self.addCleanup(auth.stop)

        def request(path, data=None, headers=None):
            extra = {"Content-Type": "application/json", **(headers or {})}
            return urlopen(
                Request(
                    base + path,
                    data=None if data is None else json.dumps(data).encode(),
                    headers=extra,
                ),
                timeout=5,
            )

        with self.assertRaises(HTTPError) as error:
            request("/reader-api/books")
        self.assertEqual(error.exception.code, 401)
        cookie = {"Cookie": "books_session=test-token"}
        with self.assertRaises(HTTPError) as error:
            request("/reader-api/collections")
        self.assertEqual(error.exception.code, 401)
        with request("/reader-api/collections", headers=cookie) as response:
            collection = json.load(response)["collections"][0]
            self.assertEqual(collection["title"], "Корейские эльфы")
        with request(collection["image"]) as response:
            self.assertEqual(response.headers.get_content_type(), "image/png")
            self.assertTrue(response.read().startswith(b"\x89PNG\r\n\x1a\n"))
        with self.assertRaises(HTTPError) as error:
            request("/reader-api/queue", {"book": "42", "device": "default"}, cookie)
        self.assertEqual(error.exception.code, 403)
        with self.assertRaises(HTTPError):
            request(
                "/reader-api/queue",
                {"book": "42", "device": "default"},
                {**cookie, "X-CSRF-Token": "csrf-test", "Origin": "https://evil.test"},
            )
        with request(
            "/reader-api/books/42/state",
            {"chapter": 3, "offset": 0.45, "shelf": "reading"},
            {**cookie, "X-CSRF-Token": "csrf-test"},
        ) as response:
            self.assertEqual(response.status, 200)
        self.assertEqual(self.app.book("alice", "42")["reading"]["chapter"], 3)
        self.assertEqual(self.app.book("bob", "42")["reading"]["chapter"], 0)
        with self.assertRaises(HTTPError) as error:
            request("/reader-api/books/42/library", {"saved": True}, cookie)
        self.assertEqual(error.exception.code, 403)
        with request(
            "/reader-api/books/42/library",
            {"saved": True, "owner": "bob"},
            {**cookie, "X-CSRF-Token": "csrf-test"},
        ) as response:
            self.assertTrue(json.load(response)["inLibrary"])
        self.assertFalse(self.app.book("bob", "42")["inLibrary"])
        with self.assertRaises(HTTPError) as error:
            request("/reader-api/books/42/favorite", {"favorite": True}, cookie)
        self.assertEqual(error.exception.code, 403)
        with request(
            "/reader-api/books/42/favorite",
            {"favorite": True, "owner": "bob"},
            {**cookie, "X-CSRF-Token": "csrf-test"},
        ) as response:
            self.assertTrue(json.load(response)["isFavorite"])
        self.assertFalse(self.app.book("bob", "42")["isFavorite"])
        with self.assertRaises(HTTPError) as error:
            request("/reader-api/books/42/download")
        self.assertEqual(error.exception.code, 401)
        with patch.object(self.app.client, "open", return_value=self.file_response()) as upstream:
            with request("/reader-api/books/42/download", headers=cookie) as response:
                self.assertEqual(response.read(), b"<FictionBook/>")
                self.assertEqual(
                    response.headers.get_content_type(), "application/x-fictionbook+xml"
                )
                self.assertTrue(response.headers["Content-Disposition"].startswith("attachment;"))
            self.assertEqual(upstream.call_count, 1)
        with request("/") as response:
            self.assertIn(b"viewport-fit=cover", response.read())
            self.assertIn("frame-ancestors 'none'", response.headers["Content-Security-Policy"])


if __name__ == "__main__":
    unittest.main()
