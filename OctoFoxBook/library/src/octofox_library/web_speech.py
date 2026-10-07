"""Owner-isolated, bounded prefetch/cache for server narration.

No credentials on disk. Unstarted personal books warm their opening; reading updates
coalesce around a stable sentence/chunk so each page turn does not synthesize again.
"""
from collections import OrderedDict
from contextlib import contextmanager
import hashlib
from html.parser import HTMLParser
import http.client
import io
import json
import logging
from pathlib import Path
import re
import socket
import sqlite3
import threading
import time
import zipfile
import wave

PROFILE = 'narrator-silero-v5.5-piper1.16-hls6-v3'
LOGGER = logging.getLogger(__name__)
VOICES = [{'voiceURI': 'eugene', 'name': 'Евгений · Silero', 'lang': 'ru-RU'},
          {'voiceURI': 'ruslan', 'name': 'Руслан · Piper', 'lang': 'ru-RU'}]
CACHE_LIMIT = 512 * 1024 * 1024
MAX_REPLY = 8 * 1024 * 1024


class SpeechError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


class TextBlocks(HTMLParser):
    """Match readerTextBlocks(), including whitespace and inline emphasis indices."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack, self.blocks, self.serial = [], [], 0

    def handle_starttag(self, tag, attrs):
        if tag not in ('br', 'img', 'hr'):
            self.serial += 1
            self.stack.append((tag, self.serial))

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                del self.stack[i:]
                break

    def handle_data(self, text):
        if not text or any(t in ('script', 'style') for t, _ in self.stack):
            return
        element = next((i for t, i in reversed(self.stack)
                        if t in ('p', 'h1', 'h2', 'h3', 'h4', 'blockquote', 'li')), 0)
        if not self.blocks or self.blocks[-1][0] != element:
            self.blocks.append([element, ''])
        self.blocks[-1][1] += text


def utf16(text):
    return len(text.encode('utf-16-le')) // 2


def narration_text(text):
    # OCR sometimes mixes Latin lookalikes into Russian words (e.g. "тиxо").
    # Silero SSML rejects these. Do not transliterate English names/whole words,
    # and never modify the displayed text or its original UTF-16 anchors.
    lookalikes = str.maketrans('ABCEHKMOPTXYacekopxy', 'АВСЕНКМОРТХУасекорху')
    def word(match):
        value = match[0]
        return value.translate(lookalikes) if re.search('[А-Яа-яЁё]', value) else value
    text = re.sub('[A-Za-zА-Яа-яЁё]+', word, text)
    # A standalone OCR preposition ("o поездке") has no Cyrillic inside it.
    # Restrict this correction to single-letter Russian words next to Cyrillic.
    text = re.sub(r'(?<!\w)([oOcCyYaAB])(?=\s+[А-Яа-яЁё])',
                  lambda m: m[0].translate(str.maketrans('oOcCyYaAB', 'оОсСуУаАВ')), text)
    return text


def page_plan(owner, book, chapter, markup, start, end, language):
    """Half-open DOM UTF-16 range. Never synthesize text from an adjacent page."""
    parser = TextBlocks(); parser.feed(markup)
    def valid(a):
        return (0 <= a['block'] < len(parser.blocks) and
                0 <= a['char'] <= utf16(parser.blocks[a['block']][1]))
    if not valid(start) or not valid(end) or (start['block'], start['char']) > (end['block'], end['char']):
        raise SpeechError(400, 'Некорректные границы страницы')
    parts = []
    for part in chapter_parts(markup, chapter):
        block = part['anchor']['block']
        if not start['block'] <= block <= end['block']:
            continue
        lo = max(part['anchor']['char'], start['char'] if block == start['block'] else 0)
        hi = min(part['end']['char'], end['char'] if block == end['block'] else part['end']['char'])
        if lo >= hi:
            continue
        raw = parser.blocks[block][1].encode('utf-16-le')[lo*2:hi*2].decode('utf-16-le', errors='ignore')
        leading = len(raw) - len(raw.lstrip())
        if not raw.strip():
            continue
        parts.append({**part, 'text': narration_text(re.sub(r'\s+', ' ', raw.strip())),
                      'anchor': {'block': block, 'char': lo + utf16(raw[:leading])},
                      'end': {'block': block, 'char': hi}})
    if sum(len(p['text']) for p in parts) > 12000:
        raise SpeechError(413, 'Слишком большая страница для озвучки')
    plan, batch, size = [], [], 0
    for part in parts:
        if batch and (size + len(part['text']) > 650 or len(batch) >= 24):
            plan.append({'chapter': chapter, 'parts': batch, 'language': language})
            batch, size = [], 0
        batch.append(part); size += len(part['text'])
    if batch:
        plan.append({'chapter': chapter, 'parts': batch, 'language': language})
    for item in plan:
        item['key'] = clip_key(owner, book, item)
    return plan


def chapter_parts(markup, chapter):
    parser = TextBlocks()
    parser.feed(markup)
    result = []
    for block, (_, text) in enumerate(parser.blocks):
        start = 0
        while start < len(text):
            while start < len(text) and text[start].isspace():
                start += 1
            if start == len(text):
                break
            end = min(len(text), start + 420)
            part = text[start:end]
            stops = list(re.finditer(r'[.!?…][»”"\']?(?:\s|$)', part))
            if stops:
                end = start + stops[-1].start() + len(stops[-1][0].rstrip())
            elif end < len(text) and part.rfind(' ') > 0:
                end = start + part.rfind(' ')
            result.append({'text': narration_text(re.sub(r'\s+', ' ', text[start:end])),
                'anchor': {'block': block, 'char': utf16(text[:start])},
                'end': {'block': block, 'char': utf16(text[:end])},
                'paragraph': not text[end:].strip(), 'chapter': chapter})
            start = end
    return result


def book_plan(owner, book, chapters, language):
    plan = []
    for chapter, value in enumerate(chapters):
        batch, size = [], 0
        for part in chapter_parts(value['html'], chapter):
            if batch and (size + len(part['text']) > 650 or len(batch) >= 24):
                plan.append({'chapter': chapter, 'parts': batch})
                batch, size = [], 0
            batch.append(part)
            size += len(part['text'])
        if batch:
            plan.append({'chapter': chapter, 'parts': batch})
    for item in plan:
        item['language'] = language
        # User/book are part of the key: neither private uploads nor audio leak across accounts.
        item['key'] = clip_key(owner, book, item)
    return plan


def clip_key(owner, book, item):
    raw = json.dumps([PROFILE, owner, book, item], ensure_ascii=False).encode()
    return hashlib.sha256(raw).hexdigest()


def trim_stream_start(plan, first, owner, book, markup, anchor):
    """Only the first clip changes; subsequent prefetched audio remains reusable.

    Page anchors refer to original DOM UTF-16 offsets, not normalized speech text.
    A stream must begin at that anchor, never at the previous 420-character cue.
    """
    entry = plan[first]
    target = (anchor['block'], anchor['char'])
    parser = TextBlocks()
    parser.feed(markup)
    parts = []
    for part in entry['parts']:
        start, end = part['anchor'], part['end']
        if (end['block'], end['char']) <= target:
            continue
        if (start['block'], start['char']) < target:
            original = parser.blocks[start['block']][1].encode('utf-16-le')
            text = original[anchor['char'] * 2:end['char'] * 2].decode('utf-16-le', errors='ignore')
            leading = len(text) - len(text.lstrip())
            if not text.strip():
                continue
            part = {**part, 'text': narration_text(re.sub(r'\s+', ' ', text[leading:])),
                    'anchor': {'block': start['block'], 'char': anchor['char'] + utf16(text[:leading])}}
        parts.append(part)
    if not parts:
        if first + 1 >= len(plan):
            raise SpeechError(422, 'После выбранной позиции нет текста для озвучки')
        return plan[:first] + plan[first + 1:]
    item = {'chapter': entry['chapter'], 'parts': parts, 'language': entry['language']}
    item['key'] = clip_key(owner, book, item)
    return plan[:first] + [item] + plan[first + 1:]


def locate(plan, chapter, anchor=None, offset=0):
    candidates = [i for i, p in enumerate(plan) if p['chapter'] == chapter]
    if not candidates:
        return next((i for i, p in enumerate(plan) if p['chapter'] > chapter), max(0, len(plan) - 1))
    if not anchor:
        return candidates[min(len(candidates) - 1, int(max(0, min(1, offset)) * len(candidates)))]
    target = (anchor.get('block', 0), anchor.get('char', 0))
    for i in candidates:
        end = plan[i]['parts'][-1]['end']
        if (end['block'], end['char']) > target:
            return i
    return candidates[-1]


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost', timeout=90)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


class BookSpeech:
    def __init__(self, app, socket_path=''):
        self.app, self.socket_path = app, socket_path
        self.root = app.database.parent / 'speech-cache'
        self.lock = threading.RLock()
        self.wishes, self.plans, self.streams = OrderedDict(), OrderedDict(), OrderedDict()
        self.warmed = {}
        self.running = False
        if not socket_path:
            return
        self.root.mkdir(mode=0o700, exist_ok=True)
        with self.db() as db:
            db.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS clips (
                    key TEXT PRIMARY KEY, owner TEXT NOT NULL, book TEXT NOT NULL,
                    payload TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'queued',
                    wanted INTEGER NOT NULL DEFAULT 0, priority INTEGER NOT NULL DEFAULT 10,
                    touched REAL NOT NULL, retry REAL NOT NULL DEFAULT 0,
                    attempts INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0);
                CREATE INDEX IF NOT EXISTS speech_pending ON clips(wanted,state,priority,retry);
                CREATE TABLE IF NOT EXISTS preferences (owner TEXT PRIMARY KEY, voice TEXT NOT NULL);
            ''')
            db.execute("UPDATE clips SET state='queued' WHERE state='working'")

    @contextmanager
    def db(self):
        db = sqlite3.connect(self.root / 'cache.sqlite3', timeout=10)
        db.row_factory = sqlite3.Row
        try:
            yield db
            db.commit()
        finally:
            db.close()

    def start(self):
        if self.socket_path and not self.running:
            self.running = True
            threading.Thread(target=self.run, daemon=True, name='book-speech').start()

    def preference(self, owner):
        if not self.socket_path:
            return 'eugene'
        with self.db() as db:
            row = db.execute('SELECT voice FROM preferences WHERE owner=?', (owner,)).fetchone()
        return row['voice'] if row and any(v['voiceURI'] == row['voice'] for v in VOICES) else 'eugene'

    def info(self, owner=''):
        return {'available': bool(self.socket_path and Path(self.socket_path).exists()),
                'voices': VOICES, 'voice': self.preference(owner), 'profile': PROFILE}

    def warm_library(self, session):
        if not self.socket_path or time.time() - self.warmed.get(session.owner, 0) < 3600:
            return
        self.warmed[session.owner] = time.time()
        with self.app.db() as db:
            rows = db.execute('SELECT book FROM personal_library WHERE owner=? ORDER BY added DESC LIMIT 100',
                              (session.owner,)).fetchall()
        for row in rows:
            self.request(session, row['book'])

    def request(self, session, book, active=False, stream=None, position=None):
        if not self.socket_path:
            if stream:
                raise SpeechError(503, 'Серверная озвучка пока недоступна')
            return
        with self.lock:
            if not stream and active and any(s['owner'] == session.owner and s['book'] == book and
                    s.get('bounded') and time.time() - s['seen'] < 1800 for s in self.streams.values()):
                return  # Page playback already queues the next page, not an unrelated whole chapter.
            key = (session.owner, book, stream or '')
            self.wishes[key] = (time.time() + (0 if stream else 3), session, book, active, stream, position)
            self.wishes.move_to_end(key, last=not bool(stream))
            while len(self.wishes) > 128:
                self.wishes.popitem(last=True)

    def forget(self, owner, book):
        if not self.socket_path:
            return
        with self.lock:
            for key in list(self.wishes):
                if key[:2] == (owner, book) and not key[2]:
                    self.wishes.pop(key)
        with self.db() as db:
            db.execute('UPDATE clips SET wanted=0 WHERE owner=? AND book=?', (owner, book))

    def prepare(self, session, book, data):
        identity = data.get('id', '')
        if not re.fullmatch(r'[a-f0-9-]{36}', str(identity)):
            raise SpeechError(400, 'Некорректный запрос озвучки')
        chapter = data.get('chapter', 0)
        voice = data.get('voice', self.preference(session.owner))
        if voice not in {v['voiceURI'] for v in VOICES}:
            raise SpeechError(400, 'Выберите русский голос из списка')
        anchor = data.get('anchor') or {'block': 0, 'char': 0}
        end = data.get('end')
        if end is not None and (not isinstance(end, dict) or set(end) != {'block', 'char'} or any(
                type(v) is not int or not 0 <= v <= 10000000 for v in end.values())):
            raise SpeechError(400, 'Некорректный конец страницы')
        if type(chapter) is not int or chapter < 0 or not isinstance(anchor, dict) or set(anchor) != {'block', 'char'} or any(
            type(v) is not int or not 0 <= v <= 10000000 for v in anchor.values()):
            raise SpeechError(400, 'Некорректная позиция озвучки')
        self.app.book(session.owner, book)
        if not self.socket_path:
            raise SpeechError(503, 'Серверная озвучка пока недоступна')
        with self.db() as db:
            db.execute('INSERT OR REPLACE INTO preferences VALUES (?,?)', (session.owner, voice))
            db.execute("UPDATE clips SET attempts=0,state='queued',retry=0 WHERE owner=? AND book=? AND state='error' AND (retry<=? OR attempts>=3)",
                       (session.owner, book, time.time()))
        with self.lock:
            old = self.streams.get(identity)
            if old:
                if old['owner'] != session.owner or old['book'] != book:
                    raise SpeechError(409, 'Озвучка уже используется')
                return {'id': identity}
            now = time.time()
            for key, stream in list(self.streams.items()):
                if now - stream['seen'] > 1800:
                    self.streams.pop(key)
            # Current page + three ahead, with room for a second tab or in-flight
            # cleanup during navigation; both per-owner and global bounds remain.
            if sum(s['owner'] == session.owner for s in self.streams.values()) >= 8 or len(self.streams) >= 24:
                raise SpeechError(429, 'Остановите другую озвучку и повторите')
            self.streams[identity] = {'owner': session.owner, 'book': book, 'seen': now,
                'session': session, 'plan': None, 'first': 0, 'requested': 0, 'anchor': anchor,
                'error': False, 'bounded': end is not None}
        self.request(session, book, True, identity, {'chapter': chapter, 'anchor': anchor, 'voice': voice, 'end': end})
        return {'id': identity}

    def stop(self, owner, identity):
        with self.lock:
            stream = self.streams.get(identity)
            if stream and stream['owner'] == owner:
                self.streams.pop(identity)
                self.wishes.pop((owner, stream['book'], identity), None)
        return {'ok': True}

    def stream(self, owner, identity):
        with self.lock:
            value = self.streams.get(identity)
            if not value or value['owner'] != owner or time.time() - value['seen'] > 1800:
                raise SpeechError(404, 'Озвучка завершена. Нажмите ▶ ещё раз')
            value['seen'] = time.time()
            return value

    def want(self, owner, book, entries, priority):
        with self.db() as db:
            for item in entries:
                db.execute('INSERT OR IGNORE INTO clips(key,owner,book,payload,touched) VALUES (?,?,?,?,?)',
                    (item['key'], owner, book, json.dumps(item, ensure_ascii=False), time.time()))
                db.execute('UPDATE clips SET wanted=1,priority=min(priority,?),touched=? WHERE key=?',
                           (priority, time.time(), item['key']))

    def ready(self, item):
        path = self.root / (item['key'] + '.json')
        try:
            return json.loads(path.read_text())
        except (OSError, ValueError):
            return None

    def status(self, owner, identity):
        stream = self.stream(owner, identity)
        if stream['error']:
            raise SpeechError(503, 'Не удалось подготовить озвучку. Попробуйте ещё раз')
        plan = stream['plan']
        if plan is None:
            return {'state': 'preparing', 'segments': []}
        if not plan and stream.get('bounded'):
            return {'state': 'ready', 'segments': [], 'complete': True, 'empty': True, 'seek': 0}
        start = stream['first']
        self.want(owner, stream['book'], plan[stream['requested']:stream['requested'] + 8], 0)
        segments, elapsed, pcm_elapsed, blocked = [], 0, 0, None
        for item in plan[start:]:
            meta = self.ready(item)
            if not meta:
                with self.db() as db:
                    row = db.execute('SELECT state,attempts,retry FROM clips WHERE key=?', (item['key'],)).fetchone()
                if row and row['state'] == 'error':
                    blocked = {'index': start + len(segments), 'failed': row['attempts'] >= 3,
                               'retryAt': row['retry']}
                break
            cues = [{**mark, 'anchor': part['anchor'], 'endAnchor': part['end']}
                    for mark, part in zip(meta['marks'], item['parts'])]
            pcm_duration = meta['marks'][-1]['end'] if meta['marks'] else meta['duration']
            segments.append({'index': start + len(segments), 'duration': meta['duration'], 'at': elapsed,
                'pcmDuration': pcm_duration, 'pcmAt': pcm_elapsed,
                'chapter': item['chapter'], 'cues': cues})
            elapsed += meta['duration']
            pcm_elapsed += pcm_duration
        if not segments:
            with self.db() as db:
                row = db.execute('SELECT state FROM clips WHERE key=?', (plan[start]['key'],)).fetchone()
            if row and row['state'] == 'error' and blocked and blocked['failed']:
                raise SpeechError(503, 'Не удалось подготовить озвучку. Попробуйте позже')
        seek = 0
        if segments:
            target = (stream['anchor']['block'], stream['anchor']['char'])
            for cue in segments[0]['cues']:
                a = cue['anchor']
                if (a['block'], a['char']) <= target:
                    seek = cue['start']
        return {'state': 'ready' if segments else 'preparing', 'segments': segments,
                'seek': seek, 'complete': start + len(segments) == len(plan), 'blocked': blocked}

    def playlist(self, owner, identity):
        # Native HLS keeps segment loading out of suspended iPhone JavaScript.
        # The initial request may race its CSRF-protected registration POST.
        deadline = time.monotonic() + 22
        while True:
            try:
                status = self.status(owner, identity)
            except SpeechError as error:
                if error.status != 404:
                    raise
                status = {'segments': []}
            bounded = self.stream(owner, identity).get('bounded') if status['segments'] else False
            if (status['segments'] and (not bounded or status['complete'])) or time.monotonic() >= deadline:
                break
            time.sleep(.2)
        if not status['segments'] or (bounded and not status['complete']):
            raise SpeechError(503, 'Подготовка озвучки. Ожидайте')
        lines = ['#EXTM3U', '#EXT-X-VERSION:4', '#EXT-X-TARGETDURATION:7',
                 '#EXT-X-MEDIA-SEQUENCE:0', '#EXT-X-PLAYLIST-TYPE:' + ('VOD' if bounded else 'EVENT'),
                 '#EXT-X-DISCONTINUITY-SEQUENCE:0', f"#EXT-X-START:TIME-OFFSET={status['seek']:.3f},PRECISE=YES"]
        plan = self.stream(owner, identity)['plan']
        count = 0
        for part in status['segments']:
            meta = self.ready(plan[part['index']])
            for slice_index, fragment in enumerate(meta['slices']):
                # One decoder timeline per synthesized clip, not a reset every
                # six seconds. Legacy cache entries retain their required tags.
                if count and (slice_index == 0 or not meta.get('continuousSlices')):
                    lines.append('#EXT-X-DISCONTINUITY')
                lines.extend([f"#EXTINF:{fragment['duration']:.6f},",
                    f"#EXT-X-BYTERANGE:{fragment['bytes']}@{fragment['offset']}", f"{part['index']}.ts"])
                count += 1
        if status['complete']:
            lines.append('#EXT-X-ENDLIST')
        return ('\n'.join(lines) + '\n').encode()

    def media(self, owner, identity, index, extension):
        stream = self.stream(owner, identity)
        plan = stream['plan']
        if plan is None or not stream['first'] <= index < len(plan) or extension not in ('wav', 'ts'):
            raise SpeechError(404, 'Фрагмент не найден')
        if not self.ready(plan[index]):
            raise SpeechError(503, 'Подготовка озвучки. Ожидайте')
        stream['requested'] = max(stream['requested'], index)
        self.want(owner, stream['book'], plan[index:index + 8], 0)
        path = self.root / (plan[index]['key'] + '.' + extension)
        if not path.is_file():
            raise SpeechError(503, 'Фрагмент нужно подготовить заново')
        return path.read_bytes()

    def page_audio(self, owner, identity):
        # The first Audio.play() must stay in the user's gesture. Its GET can
        # therefore arrive just before the protected registration POST finishes.
        deadline = time.monotonic() + 22
        while True:
            try:
                stream = self.stream(owner, identity)
                break
            except SpeechError as error:
                with self.lock:
                    known = identity in self.streams
                if error.status != 404 or known or time.monotonic() >= deadline:
                    raise
                time.sleep(.2)
        if not stream.get('bounded'):
            raise SpeechError(404, 'Страница не найдена')
        while not self.status(owner, identity).get('complete'):
            if time.monotonic() >= deadline:
                raise SpeechError(503, 'Подготовка озвучки. Ожидайте')
            time.sleep(.2)
        output = io.BytesIO()
        with wave.open(output, 'wb') as target:
            params = None
            for item in stream['plan']:
                with wave.open(str(self.root / (item['key'] + '.wav')), 'rb') as source:
                    current = (source.getnchannels(), source.getsampwidth(), source.getframerate())
                    if params is None:
                        params = current
                        target.setnchannels(params[0]); target.setsampwidth(params[1]); target.setframerate(params[2])
                    elif current != params:
                        raise SpeechError(503, 'Несовместимый аудиофрагмент')
                    target.writeframesraw(source.readframes(source.getnframes()))
            if params is None:
                # Status will skip this empty page; an already-started media GET
                # must still return a valid file while that update is in flight.
                target.setnchannels(1); target.setsampwidth(2); target.setframerate(24000)
                target.writeframesraw(b'\0\0')
        return output.getvalue()

    def _prepare(self, wish):
        _, session, book, active, identity, position = wish
        if identity and identity not in self.streams:
            return
        if identity and position and position.get('end') is not None:
            self.app.book(session.owner, book)
            chapters = self.app.chapters(session, book)
            chapter = position['chapter']
            if chapter >= len(chapters):
                raise SpeechError(400, 'Глава не найдена')
            plan = page_plan(session.owner, book, chapter, chapters[chapter]['html'],
                             position['anchor'], position['end'], position['voice'])
            with self.lock:
                stream = self.streams.get(identity)
                if not stream:
                    return
                stream.update(plan=plan, first=0, requested=0)
            self.want(session.owner, book, plan, 0)
            return
        with self.lock:
            voice = (position or {}).get('voice', self.preference(session.owner))
            plan = self.plans.get((session.owner, book, voice))
        info = self.app.book(session.owner, book)
        if not identity and not info['inLibrary']:
            return
        if plan is None:
            plan = book_plan(session.owner, book, self.app.chapters(session, book), voice)
            if not plan:
                raise SpeechError(422, 'Нет текста для озвучки')
            with self.lock:
                self.plans[(session.owner, book, voice)] = plan
                while len(self.plans) > 3:
                    self.plans.popitem(last=False)
        position = position or info['reading']
        first = locate(plan, position.get('chapter', 0), position.get('anchor'), position.get('offset', 0))
        if identity:
            anchor = position.get('anchor')
            beginning = plan[first]['parts'][0]['anchor']
            if anchor and plan[first]['chapter'] == position.get('chapter', 0) and (
                    anchor['block'], anchor['char']) > (beginning['block'], beginning['char']):
                plan = trim_stream_start(plan, first, session.owner, book,
                    self.app.chapters(session, book)[plan[first]['chapter']]['html'], anchor)
            with self.lock:
                stream = self.streams.get(identity)
                if not stream:
                    return
                stream.update(plan=plan, first=first, requested=first)
        self.want(session.owner, book, plan[first:first + (8 if active else 4)], 0 if identity else 2)
        if active:
            next_chapter = plan[first]['chapter'] + 1
            # Prefetch the next chapter at lower priority, capped for abnormally long chapters.
            self.want(session.owner, book, [p for p in plan if p['chapter'] == next_chapter][:64], 5)

    def render_one(self):
        with self.db() as db:
            row = db.execute("SELECT * FROM clips WHERE wanted=1 AND state IN ('queued','error') "
                "AND retry<=? AND attempts<3 ORDER BY priority,touched ASC LIMIT 1", (time.time(),)).fetchone()
            if not row:
                return False
            db.execute("UPDATE clips SET state='working',attempts=attempts+1 WHERE key=?", (row['key'],))
        try:
            item = json.loads(row['payload'])
            conn = UnixHTTP(self.socket_path)
            try:
                conn.request('POST', '/synthesize', json.dumps(item).encode(), {'Content-Type': 'application/json'})
                response = conn.getresponse()
                payload = response.read(MAX_REPLY + 1)
                if response.status != 200 or len(payload) > MAX_REPLY:
                    raise ValueError('Speech worker failed')
            finally:
                conn.close()
            with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                if set(archive.namelist()) != {'audio.wav', 'audio.ts', 'meta.json'} or sum(i.file_size for i in archive.infolist()) > MAX_REPLY:
                    raise ValueError('Invalid speech output')
                wav, ts, meta = (archive.read(n) for n in ('audio.wav', 'audio.ts', 'meta.json'))
            data = json.loads(meta)
            if not wav.startswith(b'RIFF') or not ts or ts[0] != 0x47 or not 0 < data['duration'] < 120 or len(data['marks']) != len(item['parts']):
                raise ValueError('Invalid audio')
            offset = 0
            for fragment in data['slices']:
                if fragment['offset'] != offset or fragment['bytes'] <= 0 or not 0 < fragment['duration'] < 7:
                    raise ValueError('Invalid HLS fragment')
                offset += fragment['bytes']
            if offset != len(ts) or abs(sum(p['duration'] for p in data['slices']) - data['duration']) > .001:
                raise ValueError('Invalid HLS ranges')
            for suffix, content in [('wav', wav), ('ts', ts), ('json', meta)]:
                target = self.root / (row['key'] + '.' + suffix)
                temporary = target.with_suffix('.tmp')
                temporary.write_bytes(content)
                temporary.replace(target)  # Metadata last: clients never see a half-ready clip.
            with self.db() as db:
                db.execute("UPDATE clips SET state='ready',bytes=?,touched=? WHERE key=?",
                           (len(wav) + len(ts) + len(meta), time.time(), row['key']))
            self.prune()
        except Exception as error:
            LOGGER.warning('Speech clip failed: book=%s key=%s type=%s',
                           row['book'], row['key'], type(error).__name__)
            with self.db() as db:
                db.execute("UPDATE clips SET state='error',retry=? WHERE key=?",
                           (time.time() + (3 if row['attempts'] == 0 else 15), row['key']))
        return True

    def prune(self):
        with self.lock:
            protected = {p['key'] for s in self.streams.values() if time.time() - s['seen'] < 1800
                         for p in (s['plan'] or [])[s['first']:s['requested'] + 8]}
        with self.db() as db:
            rows = db.execute('SELECT key,bytes,touched FROM clips ORDER BY touched').fetchall()
            total = sum(r['bytes'] for r in rows)
            for row in rows:
                if row['key'] in protected or (total <= CACHE_LIMIT and time.time() - row['touched'] < 7 * 86400):
                    continue
                if not re.fullmatch(r'[a-f0-9]{64}', row['key']):
                    continue
                for suffix in ('wav', 'ts', 'json'):
                    (self.root / (row['key'] + '.' + suffix)).unlink(missing_ok=True)
                db.execute('DELETE FROM clips WHERE key=?', (row['key'],))
                total -= row['bytes']

    def run(self):
        while self.running:
            wish = None
            with self.lock:
                for key, value in self.wishes.items():
                    if value[0] <= time.time():
                        wish = self.wishes.pop(key)
                        break
            if wish:
                try:
                    self._prepare(wish)
                except Exception:
                    if wish[4] in self.streams:
                        self.streams[wish[4]]['error'] = True
            try:
                worked = self.render_one()
            except Exception:
                worked = False
            if not worked:
                time.sleep(.25)
