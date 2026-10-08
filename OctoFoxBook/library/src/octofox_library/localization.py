"""Localize application-owned resources without rewriting user or book content.

The source UI remains Russian. The English catalog translates static source
fragments before serving HTML/JS, preserving the existing markup and handlers.
Coverage and JavaScript parsing are checked for every localized asset in tests.
No runtime DOM observer, external translation service or book-text substitution.
"""
from functools import lru_cache
from http.cookies import SimpleCookie
import json
from pathlib import Path
import re

WEB = Path(__file__).with_name('web')
FRAGMENT = re.compile(r'[А-Яа-яЁё][^<>\n\r"\'`{}$\\]*[А-Яа-яЁё…!?.,:;→↗↓▶×0-9)]|[А-Яа-яЁё]+')


@lru_cache(maxsize=1)
def catalog():
    return json.loads((WEB / 'en.json').read_text('utf-8'))


def language(headers):
    cookies = SimpleCookie()
    try:
        cookies.load(headers.get('Cookie', ''))
        saved = cookies.get('octofox_lang')
        if saved and saved.value in {'en', 'ru'}:
            return saved.value
    except Exception:
        pass
    choices = []
    for index, item in enumerate(headers.get('Accept-Language', '').split(',')):
        locale, *parameters = item.strip().lower().split(';')
        try:
            quality = next((float(p.strip()[2:]) for p in parameters if p.strip().startswith('q=')), 1)
        except ValueError:
            continue
        if locale.split('-')[0] in {'en', 'ru'} and 0 < quality <= 1:
            choices.append((quality, -index, locale.split('-')[0]))
    return max(choices)[2] if choices else 'ru'


@lru_cache(maxsize=64)
def asset(name, locale):
    data = (WEB / name).read_bytes()
    if locale != 'en' or Path(name).suffix not in {'.html', '.js', '.webmanifest'}:
        return data
    strings = catalog()['assets']
    text = data.decode('utf-8')
    def replace(match):
        original = match[0]
        return strings.get(original, original)
    text = FRAGMENT.sub(replace, text)
    if name.endswith('.html'):
        text = text.replace('lang="ru"', 'lang="en"')
    if name.endswith('.js'):
        text = text.replace('toLocaleString("ru"', 'toLocaleString("en"')
        text = text.replace('.toLocaleTimeString()', '.toLocaleTimeString("en")')
    return text.encode('utf-8')


def message(text, locale):
    if locale != 'en' or not isinstance(text, str):
        return text
    translated = catalog()['messages'].get(text)
    if translated is not None:
        return translated
    # Known setup recovery message combines a fixed prefix, a public error and
    # a fixed suffix. Translate those parts without handling arbitrary metadata.
    prefix = 'Аккаунт создан. '
    suffix = ' Используйте «Завершить подключение» с теми же логином и паролем.'
    if text.startswith(prefix) and text.endswith(suffix):
        return ('Account created. ' + message(text[len(prefix):-len(suffix)], locale)
                + ' Use Finish connecting with the same username and password.')
    if re.search('[А-Яа-яЁё]', text):
        return 'Could not complete the action. Check the current state and try again.'
    return text


def taxonomy(label, locale):
    return catalog()['taxonomy'].get(label, label) if locale == 'en' else label


def chapters(items, locale):
    if locale != 'en':
        return items
    return [dict(item, title=f'Part {index + 1}') if item.get('generatedTitle') else item
            for index, item in enumerate(items)]


def book(value, locale):
    if not isinstance(value, dict) or 'genreItems' not in value:
        return value
    return dict(value, genreItems=[dict(item, label=taxonomy(item['label'], locale))
                                  for item in value['genreItems']])


def response(value, locale):
    if locale != 'en' or not isinstance(value, dict):
        return value
    result = dict(value)
    for field in ('error', 'message'):
        if isinstance(result.get(field), str):
            result[field] = message(result[field], locale)
    if 'voices' in result:
        names = {'eugene': 'Russian · Eugene · Silero', 'ruslan': 'Russian · Ruslan · Piper'}
        result['voices'] = [dict(v, name=names.get(v['voiceURI'], v['name'])) for v in result['voices']]
    result = book(result, locale)
    for field in ('books', 'items'):
        if isinstance(result.get(field), list):
            result[field] = [book(item, locale) for item in result[field]]
    if isinstance(result.get('collections'), list):
        collections = []
        for item in result['collections']:
            item = dict(item)
            if item.get('id') == 'korean-elves' and item.get('image') == '/korean-elves.png':
                item.update(title='Korean Elves', description='K-dramas, romance, cozy stories and Korean magic.')
            if str(item.get('id', '')).startswith('uploads-') and item.get('name') == 'Загруженные':
                item['name'] = 'Uploads'
            collections.append(item)
        result['collections'] = collections
    return result
