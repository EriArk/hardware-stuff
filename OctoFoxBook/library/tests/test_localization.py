import ast
import json
from pathlib import Path
import re
import unittest

from octofox_library import localization
from test_companion import CompanionHTTPCase


class LocalizationTests(unittest.TestCase):
    def test_language_cookie_and_weighted_accept_language(self):
        self.assertEqual(localization.language({}), 'ru')
        self.assertEqual(localization.language({'Accept-Language': 'fr, en-US;q=0.8,ru;q=0.3'}), 'en')
        self.assertEqual(localization.language({'Accept-Language': 'en;q=0,ru;q=0.2'}), 'ru')
        self.assertEqual(localization.language({'Cookie': 'octofox_lang=ru', 'Accept-Language': 'en-US'}), 'ru')
        self.assertEqual(localization.language({'Cookie': 'octofox_lang=en', 'Accept-Language': 'ru'}), 'en')
        self.assertEqual(localization.language({'Cookie': 'octofox_lang=../../file'}), 'ru')

    def test_all_english_assets_have_complete_catalog_coverage(self):
        for file in localization.WEB.iterdir():
            if file.suffix not in {'.html', '.js', '.webmanifest'}:
                continue
            with self.subTest(file=file.name):
                rendered = localization.asset(file.name, 'en').decode('utf-8')
                self.assertIsNone(re.search('[А-Яа-яЁё]', rendered), file.name)
                if file.suffix == '.html':
                    self.assertIn('lang="en"', rendered)
                self.assertEqual(localization.asset(file.name, 'ru'), file.read_bytes())

    def test_errors_have_explicit_translations(self):
        messages = localization.catalog()['messages']
        for path in localization.WEB.parent.glob('*.py'):
            tree = ast.parse(path.read_text('utf-8'))
            for node in ast.walk(tree):
                if not isinstance(node, ast.Raise) or not isinstance(node.exc, ast.Call):
                    continue
                for arg in node.exc.args:
                    if isinstance(arg, ast.Constant) and isinstance(arg.value, str) and re.search('[А-Яа-яЁё]', arg.value):
                        self.assertIn(arg.value, messages, path.name)

    def test_response_does_not_translate_user_names_books_or_book_text(self):
        value = {'title': 'Моя библиотека', 'name': 'Читатель', 'html': '<p>Все книги</p>',
                 'collections': [{'name': 'Книга'}], 'error': 'Книга не найдена'}
        result = localization.response(value, 'en')
        self.assertEqual(result['error'], 'Book not found')
        for field in ('title', 'name', 'html', 'collections'):
            self.assertEqual(value[field], result[field])
        self.assertEqual(value['error'], 'Книга не найдена')

    def test_owned_labels_and_generated_chapters_preserve_user_content(self):
        from octofox_library.web_catalog import GENRES, GENRE_CATEGORIES, TAG_CATEGORIES
        for label in [v[1] for v in GENRES.values()] + list(GENRE_CATEGORIES.values()) + list(TAG_CATEGORIES.values()):
            self.assertIsNone(re.search('[А-Яа-яЁё]', localization.taxonomy(label, 'en')), label)
        value = {'books': [{'title': 'Фэнтези', 'genreItems': [{'value': 'sf_fantasy', 'label': 'Фэнтези'}],
                            'tagItems': [{'label': 'Магия'}]}],
                 'collections': [{'id': 'uploads-abc', 'name': 'Загруженные'},
                                 {'id': 'custom', 'name': 'Загруженные'},
                                 {'id': 'uploads-def', 'name': 'Мои книги'}]}
        result = localization.response(value, 'en')
        self.assertEqual(result['books'][0]['genreItems'][0]['label'], 'Fantasy')
        self.assertEqual(result['books'][0]['title'], 'Фэнтези')
        self.assertEqual(result['books'][0]['tagItems'], value['books'][0]['tagItems'])
        self.assertEqual([c['name'] for c in result['collections']], ['Uploads', 'Загруженные', 'Мои книги'])
        chapters = [{'title': 'Часть 1', 'generatedTitle': False, 'html': '<p>Привет</p>'},
                    {'title': 'Часть 2', 'generatedTitle': True, 'html': '<p>Hello</p>'}]
        translated = localization.chapters(chapters, 'en')
        self.assertEqual([c['title'] for c in translated], ['Часть 1', 'Part 2'])
        self.assertEqual(translated[1]['html'], chapters[1]['html'])

    def test_genre_search_and_alphabet_follow_interface_language(self):
        from test_books_web import BooksWebTest
        fixture = BooksWebTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        result = fixture.app.facets('alice', {'kind': ['genre'], 'q': ['heroic'], 'letter': ['H']}, 'en')
        self.assertEqual(result['total'], 1)
        self.assertEqual(result['letters'], ['H'])
        self.assertEqual(result['items'][0]['value'], 'sf_heroic')
        self.assertEqual(result['items'][0]['label'], 'Heroic science fiction')
        self.assertEqual(fixture.app.facets('alice', {'q': ['heroic']})['total'], 0)
        self.assertEqual(fixture.app.facets('alice', {'q': ['Героическая']})['total'], 1)


class LocalizationHTTPTests(CompanionHTTPCase):
    def test_english_api_errors_and_russian_cookie_override(self):
        status, data, headers = self.call('/companion-api/network', headers={'Accept-Language': 'en-US'})
        self.assertEqual(status, 401)
        self.assertEqual(data['error'], 'Sign in to Companion.')
        self.assertEqual(headers['Vary'], 'Cookie, Accept-Language')
        status, data, _ = self.call('/companion-api/network', headers={'Accept-Language': 'en-US', 'Cookie': 'octofox_lang=ru'})
        self.assertEqual(data['error'], 'Войдите в Companion.')
