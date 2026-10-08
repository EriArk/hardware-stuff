import io
import json
from pathlib import Path
import tempfile
import time
import threading
import unittest
from unittest.mock import patch
import uuid
import zipfile
import wave
from urllib.request import Request, urlopen
from urllib.error import HTTPError

from octofox_library.books_web import LibraryWeb, Session, WebServer
from octofox_library.web_speech import BookSpeech, SpeechError, book_plan, chapter_parts, locate, narration_text, page_plan, VOICES


class SpeechTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.app = LibraryWeb(Path(self.tmp.name) / 'web.sqlite3', 'http://localhost/api/v1/opds', 'http://localhost')
        self.speech = BookSpeech(self.app, '/test/socket')
        self.session = Session('alice', 'Basic private', 'csrf', time.time() + 1000)
        self.chapters = [{'html': '<p>' + ('Длинное предложение про книгу. ' * 300) + '</p>'},
                         {'html': '<p>' + ('Следующая глава ждёт читателя. ' * 300) + '</p>'}]
        self.info = {'inLibrary': True, 'reading': {'chapter': 0, 'offset': 0}}
        book = patch.object(self.app, 'book', return_value=self.info)
        self.book = book.start(); self.addCleanup(book.stop)
        chapters = patch.object(self.app, 'chapters', return_value=self.chapters)
        chapters.start(); self.addCleanup(chapters.stop)

    def prepare(self, **data):
        identity = str(uuid.uuid4())
        self.speech.prepare(self.session, '42', {'id': identity, **data})
        wish = self.speech.wishes.pop((self.session.owner, '42', identity))
        self.speech._prepare(wish)
        return identity

    def ready(self, entry, duration=10):
        meta = {'duration': duration, 'slices': [{'duration': 5, 'offset': 0, 'bytes': 2},
                {'duration': 5, 'offset': 2, 'bytes': 3}], 'marks': [{'start': i * 2, 'end': i * 2 + 2}
                 for i in range(len(entry['parts']))]}
        (self.speech.root / (entry['key'] + '.json')).write_text(json.dumps(meta))
        (self.speech.root / (entry['key'] + '.wav')).write_bytes(b'RIFFtest')
        (self.speech.root / (entry['key'] + '.ts')).write_bytes(b'Gtest')

    def test_inline_whitespace_and_utf16_anchors(self):
        parts = chapter_parts('<p>  Раз <strong>два</strong>. 😀 Потом!</p>\n<p>Конец.</p>', 3)
        self.assertEqual(parts[0]['text'], 'Раз два. 😀 Потом!')
        self.assertEqual(parts[0]['anchor'], {'block': 0, 'char': 2})
        self.assertEqual(parts[0]['end']['char'], 20)
        self.assertEqual(parts[1]['anchor']['block'], 2)  # Inter-paragraph DOM whitespace is a block.
        self.assertEqual(parts[1]['chapter'], 3)

    def test_page_range_has_no_previous_or_next_page_text(self):
        text = '<p>😀 Начало. Середина. Конец.</p><p>Дальше.</p>'
        plan = page_plan('alice', '42', 0, text, {'block':0,'char':11}, {'block':0,'char':20}, 'eugene')
        self.assertEqual(plan[0]['parts'][0]['text'], 'Середина.')
        self.assertEqual(plan[0]['parts'][0]['anchor'], {'block':0,'char':11})
        self.assertEqual(plan[-1]['parts'][-1]['end'], {'block':0,'char':20})
        self.assertNotEqual(plan[0]['key'], page_plan('bob', '42', 0, text,
            {'block':0,'char':11}, {'block':0,'char':20}, 'eugene')[0]['key'])

    def test_page_playlist_is_finite_vod_not_live_whole_book(self):
        self.chapters[:] = [{'html':'<p>Первая страница. Вторая страница.</p>'}]
        identity = self.prepare(anchor={'block':0,'char':0}, end={'block':0,'char':16})
        plan = self.speech.streams[identity]['plan']
        self.assertEqual(len(plan), 1)
        self.assertEqual(plan[0]['parts'][0]['text'], 'Первая страница.')
        self.ready(plan[0])
        playlist = self.speech.playlist('alice', identity)
        self.assertIn(b'#EXT-X-PLAYLIST-TYPE:VOD', playlist)
        self.assertIn(b'#EXT-X-ENDLIST', playlist)
        self.assertTrue(self.speech.status('alice', identity)['complete'])
        with self.assertRaises(SpeechError):
            self.speech.page_audio('bob', identity)

    def test_ocr_single_letter_preposition_but_not_english_words(self):
        self.assertEqual(narration_text('o поездке. c ним. Apple Ridero ISBN.'), 'о поездке. с ним. Apple Ridero ISBN.')

    def test_invalid_page_end_is_rejected(self):
        for end in ({'block':-1,'char':0}, {'block':0,'char':True}, 'bad'):
            with self.assertRaises(SpeechError): self.prepare(end=end)
        with self.assertRaises(SpeechError):
            page_plan('alice','42',0,'<p>Текст</p>',{'block':0,'char':3},{'block':0,'char':1},'eugene')

    def test_complete_page_wav_joins_audio_not_riff_headers(self):
        self.chapters[:] = [{'html':'<p>' + 'абв ' * 300 + '</p>'}]
        identity = self.prepare(end={'block':0,'char':1200})
        plan = self.speech.streams[identity]['plan']
        self.assertGreater(len(plan),1)
        for i,item in enumerate(plan):
            self.ready(item)
            with wave.open(str(self.speech.root/(item['key']+'.wav')), 'wb') as wav:
                wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(24000)
                wav.writeframes(bytes([i,0])*240)
        data = self.speech.page_audio('alice',identity)
        with wave.open(io.BytesIO(data)) as wav:
            self.assertEqual(wav.getnframes(),240*len(plan))
            self.assertEqual(wav.getframerate(),24000)

    def test_pcm_timeline_excludes_aac_padding_between_clips(self):
        identity = self.prepare(end={'block':0,'char':1200})
        plan = self.speech.streams[identity]['plan']
        self.assertGreater(len(plan), 1)
        for item in plan:
            self.ready(item, duration=10.3)
        status = self.speech.status('alice', identity)
        self.assertEqual(status['segments'][1]['at'], 10.3)
        first_pcm = status['segments'][0]['cues'][-1]['end']
        self.assertEqual(status['segments'][0]['pcmDuration'], first_pcm)
        self.assertEqual(status['segments'][1]['pcmAt'], first_pcm)

    def test_first_page_audio_waits_for_its_registration(self):
        identity = self.prepare(end={'block':0,'char':10})
        stream = self.speech.streams.pop(identity)
        item = stream['plan'][0]
        with wave.open(str(self.speech.root / (item['key'] + '.wav')), 'wb') as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(24000)
            wav.writeframes(b'\0\0' * 240)
        with patch.object(self.speech, 'stream', side_effect=[SpeechError(404, 'pending'), stream]), \
             patch.object(self.speech, 'status', return_value={'complete': True}), \
             patch('octofox_library.web_speech.time.sleep') as sleep:
            data = self.speech.page_audio('alice', identity)
            self.assertTrue(data.startswith(b'RIFF'))
            sleep.assert_called_once_with(.2)

    def test_empty_page_audio_is_valid_while_client_skips_it(self):
        self.chapters[:] = [{'html':'<p>   </p>'}]
        identity = self.prepare(end={'block':0,'char':3})
        self.assertTrue(self.speech.status('alice', identity)['empty'])
        with wave.open(io.BytesIO(self.speech.page_audio('alice', identity))) as wav:
            self.assertEqual(wav.getnframes(), 1)

    def test_limits_resume_and_owner_voice_isolation(self):
        plan = book_plan('alice', '42', self.chapters, 'eugene')
        self.assertTrue(all(sum(len(p['text']) for p in x['parts']) <= 650 for x in plan))
        self.assertTrue(all(len(p['text']) <= 420 for x in plan for p in x['parts']))
        self.assertEqual(locate(plan, 0, plan[2]['parts'][0]['anchor']), 2)
        self.assertNotEqual(plan[0]['key'], book_plan('bob', '42', self.chapters, 'eugene')[0]['key'])
        self.assertNotEqual(plan[0]['key'], book_plan('alice', '42', self.chapters, 'ruslan')[0]['key'])

    def test_stream_capacity_allows_three_pages_ahead_and_keeps_owner_limit(self):
        streams = [self.prepare(end={'block':0,'char':20}) for _ in range(8)]
        with self.assertRaises(SpeechError) as caught:
            self.prepare(end={'block':0,'char':20})
        self.assertEqual(caught.exception.status, 429)
        self.speech.stop('alice', streams[0])
        self.prepare(end={'block':0,'char':20})
        self.assertEqual(len(self.speech.streams), 8)

    def test_lookahead_keeps_global_stream_capacity_bounded(self):
        for owner in ('alice', 'bob', 'carol'):
            self.session = Session(owner, 'Basic private', 'csrf', time.time() + 1000)
            for _ in range(8):
                self.prepare(end={'block':0,'char':20})
        self.session = Session('dave', 'Basic private', 'csrf', time.time() + 1000)
        with self.assertRaises(SpeechError) as caught:
            self.speech.prepare(self.session, '42', {'id':str(uuid.uuid4()), 'end':{'block':0,'char':20}})
        self.assertEqual(caught.exception.status, 429)

    def test_mixed_alphabet_ocr_is_spoken_without_changing_original_anchors(self):
        self.assertEqual(narration_text('Тиxо. Xорошо. Apple и SpaceX.'), 'Тихо. Хорошо. Apple и SpaceX.')
        parts = chapter_parts('<p>😀 Тиxо.</p><p>Apple.</p>', 0)
        self.assertEqual(parts[0]['text'], '😀 Тихо.')
        self.assertEqual(parts[0]['end'], {'block': 0, 'char': 8})
        self.assertEqual(parts[1]['text'], 'Apple.')

    def test_error_after_ready_audio_is_reported_and_explicit_play_can_retry(self):
        identity = self.prepare()
        plan = self.speech.streams[identity]['plan']
        self.ready(plan[0])
        with self.speech.db() as db:
            db.execute("UPDATE clips SET state='error',attempts=3,retry=? WHERE key=?", (time.time() + 15, plan[1]['key']))
        result = self.speech.status('alice', identity)
        self.assertEqual(result['blocked']['index'], 1)
        self.assertTrue(result['blocked']['failed'])
        self.assertFalse(result['complete'])
        self.prepare()
        with self.speech.db() as db:
            row = db.execute('SELECT attempts,state,retry FROM clips WHERE key=?', (plan[1]['key'],)).fetchone()
        self.assertEqual(tuple(row), (0, 'queued', 0))

    def test_transient_error_keeps_stream_preparing_during_short_retry(self):
        identity = self.prepare()
        first = self.speech.streams[identity]['plan'][0]
        with patch('octofox_library.web_speech.UnixHTTP', side_effect=OSError('offline')):
            self.speech.render_one()
        result = self.speech.status('alice', identity)
        self.assertEqual(result['state'], 'preparing')
        self.assertFalse(result['blocked']['failed'])
        self.assertLess(result['blocked']['retryAt'], time.time() + 4)

    def test_continuous_transport_slices_only_discontinue_between_synthesized_clips(self):
        identity = self.prepare()
        for entry in self.speech.streams[identity]['plan'][:2]:
            self.ready(entry)
            path = self.speech.root / (entry['key'] + '.json')
            meta = json.loads(path.read_text()); meta['continuousSlices'] = True
            path.write_text(json.dumps(meta))
        playlist = self.speech.playlist('alice', identity)
        self.assertEqual(playlist.count(b'#EXTINF:'), 4)
        self.assertEqual(playlist.count(b'#EXT-X-DISCONTINUITY\n'), 1)
        self.assertNotIn(b'#EXT-X-ENDLIST', playlist)

    def test_multilingual_choices_and_strict_input(self):
        self.assertEqual({v['lang'] for v in VOICES}, {'ru-RU', 'en-US', 'de-DE', 'fr-FR', 'es-ES', 'pt-BR'})
        self.assertEqual([v['voiceURI'] for v in VOICES], ['eugene', 'ruslan', 'ljspeech', 'thorsten', 'siwis', 'davefx', 'faber'])
        for data in ({'voice': 'en'}, {'voice': 'kseniya'}, {'chapter': True}, {'anchor': {'block': -1, 'char': 0}}):
            with self.assertRaises(SpeechError):
                self.speech.prepare(self.session, '42', {'id': str(uuid.uuid4()), **data})
        self.prepare(voice='ruslan')
        self.assertEqual(BookSpeech(self.app, '/test/socket').preference('alice'), 'ruslan')
        self.assertEqual(self.speech.preference('bob'), 'eugene')

    def test_each_new_voice_keeps_text_anchors_and_has_separate_audio_cache(self):
        text = 'Café, Straße, coração, mañana. 😀 O мир stays unchanged.'
        markup = '<p>' + text + '</p>'
        self.chapters[:] = [{'html': markup}]
        keys = set()
        for voice in ('ljspeech', 'thorsten', 'siwis', 'davefx', 'faber'):
            identity = self.prepare(voice=voice, anchor={'block': 0, 'char': 0},
                                    end={'block': 0, 'char': len(text.encode('utf-16-le')) // 2})
            plan = self.speech.streams[identity]['plan']
            self.assertEqual(' '.join(p['text'] for p in plan[0]['parts']), text)
            self.assertEqual(plan[0]['language'], voice)
            keys.add(plan[0]['key'])
            self.assertEqual(BookSpeech(self.app, '/test/socket').preference('alice'), voice)
            self.speech.stop('alice', identity)
        self.assertEqual(len(keys), 5)

    def test_retired_voice_preference_falls_back_without_losing_other_preferences(self):
        with patch('octofox_library.web_speech.VOICES', VOICES + [{'voiceURI': 'kseniya'}]):
            self.prepare(voice='kseniya')
        restarted = BookSpeech(self.app, '/test/socket')
        self.assertEqual(restarted.preference('alice'), 'eugene')
        self.assertEqual(restarted.info('alice')['voice'], 'eugene')
        self.prepare(voice='ruslan')
        self.assertEqual(restarted.preference('alice'), 'ruslan')

    def test_personal_opening_only_and_saved_position_next_chapter(self):
        self.speech._prepare((0, self.session, '42', False, None, None))
        with self.speech.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM clips').fetchone()[0], 4)
        plan = book_plan('alice', '42', self.chapters, 'eugene')
        self.info['reading']['anchor'] = plan[5]['parts'][0]['anchor']
        self.speech._prepare((0, self.session, '42', True, None, None))
        with self.speech.db() as db:
            rows = db.execute('SELECT payload,priority FROM clips').fetchall()
        self.assertTrue(any(json.loads(r['payload'])['key'] == plan[5]['key'] for r in rows))
        self.assertTrue(any(json.loads(r['payload'])['chapter'] == 1 and r['priority'] == 5 for r in rows))
        self.assertLess(len(rows), len(plan))

    def test_not_personal_no_background_but_explicit_listen_allowed(self):
        self.info['inLibrary'] = False
        self.speech._prepare((0, self.session, '42', True, None, None))
        with self.speech.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM clips').fetchone()[0], 0)
        self.prepare()
        with self.speech.db() as db:
            self.assertGreater(db.execute('SELECT count(*) FROM clips').fetchone()[0], 0)

    def test_coalescing_and_removal(self):
        for _ in range(20):
            self.speech.request(self.session, '42', active=True)
        self.assertEqual(len(self.speech.wishes), 1)
        self.speech._prepare(next(iter(self.speech.wishes.values())))
        self.speech.forget('alice', '42')
        self.assertFalse(self.speech.wishes)
        with self.speech.db() as db:
            self.assertEqual(db.execute('SELECT sum(wanted) FROM clips').fetchone()[0], 0)

    def test_stream_hls_is_append_only_and_owner_scoped(self):
        identity = self.prepare()
        stream = self.speech.stream('alice', identity)
        self.assertEqual(self.speech.status('alice', identity)['state'], 'preparing')
        self.ready(stream['plan'][0])
        first = self.speech.playlist('alice', identity)
        self.assertIn(b'#EXT-X-PLAYLIST-TYPE:EVENT', first)
        self.assertIn(b'0.ts', first)
        self.assertNotIn(b'#EXT-X-ENDLIST', first)
        self.ready(stream['plan'][1])
        second = self.speech.playlist('alice', identity)
        self.assertTrue(second.startswith(first))
        self.assertIn(b'#EXT-X-DISCONTINUITY\n', second)
        self.assertEqual(self.speech.media('alice', identity, 0, 'wav'), b'RIFFtest')
        with self.assertRaises(SpeechError) as caught:
            self.speech.media('bob', identity, 0, 'wav')
        self.assertEqual(caught.exception.status, 404)
        self.speech.stop('bob', identity)
        self.assertIn(identity, self.speech.streams)
        self.speech.stop('alice', identity)
        with self.assertRaises(SpeechError):
            self.speech.status('alice', identity)

    def test_finished_playlist_and_sentence_seek(self):
        self.chapters[:] = [{'html': '<p>Начало.</p><p>Продолжение.</p>'}]
        identity = self.prepare(anchor={'block': 1, 'char': 0})
        self.ready(self.speech.streams[identity]['plan'][0])
        result = self.speech.status('alice', identity)
        self.assertTrue(result['complete'])
        self.assertEqual(result['seek'], 0)
        self.assertEqual(result['segments'][0]['cues'][0]['anchor'], {'block': 1, 'char': 0})
        self.assertIn(b'#EXT-X-ENDLIST', self.speech.playlist('alice', identity))

    def test_start_inside_cached_phrase_trims_only_first_clip_and_keeps_utf16_anchor(self):
        text = '😀 Начало.   Здесь <b>начинается</b> новая страница. ' + 'Продолжаем читать книгу. ' * 80
        self.chapters[:] = [{'html': '<p>' + text + '</p>'}]
        identity = self.prepare(anchor={'block': 0, 'char': 13})
        stream = self.speech.streams[identity]
        plan = stream['plan']; first = plan[stream['first']]
        base = self.speech.plans[('alice', '42', 'eugene')]
        self.assertEqual(first['parts'][0]['anchor'], {'block': 0, 'char': 13})
        self.assertTrue(first['parts'][0]['text'].startswith('Здесь начинается новая страница.'))
        self.assertEqual(first['parts'][-1]['end'], base[0]['parts'][-1]['end'])
        self.assertNotEqual(first['key'], base[0]['key'])
        self.assertEqual(plan[1]['key'], base[1]['key'])
        self.assertEqual(base[0]['parts'][0]['anchor'], {'block': 0, 'char': 0})
        self.ready(first); self.ready(plan[1])
        status = self.speech.status('alice', identity)
        self.assertEqual(status['seek'], 0)
        self.assertEqual(status['segments'][0]['cues'][0]['anchor'], {'block': 0, 'char': 13})
        self.assertIn(b'#EXT-X-START:TIME-OFFSET=0.000,PRECISE=YES', self.speech.playlist('alice', identity))

    def test_second_stream_start_cannot_mutate_the_previous_stream(self):
        first_id = self.prepare(anchor={'block': 0, 'char': 80})
        old_plan = json.dumps(self.speech.streams[first_id]['plan'])
        next_id = self.prepare(anchor={'block': 0, 'char': 160})
        self.assertEqual(json.dumps(self.speech.streams[first_id]['plan']), old_plan)
        self.assertNotEqual(self.speech.streams[first_id]['plan'][0]['key'], self.speech.streams[next_id]['plan'][0]['key'])

    def test_render_first_chunk_first_and_restart_recovery(self):
        identity = self.prepare()
        entry = self.speech.streams[identity]['plan'][0]
        result = io.BytesIO()
        with zipfile.ZipFile(result, 'w') as archive:
            archive.writestr('audio.wav', b'RIFFtest')
            archive.writestr('audio.ts', b'Gtest')
            archive.writestr('meta.json', json.dumps({'duration': 5,
                'slices': [{'duration': 5, 'offset': 0, 'bytes': 5}], 'marks':
                [{'start': 0, 'end': 10} for _ in entry['parts']]}))
        with patch('octofox_library.web_speech.UnixHTTP') as conn:
            response = conn.return_value.getresponse.return_value
            response.status = 200; response.read.return_value = result.getvalue()
            self.assertTrue(self.speech.render_one())
        self.assertIsNotNone(self.speech.ready(entry))
        with self.speech.db() as db:
            db.execute("UPDATE clips SET state='working' WHERE state='queued'")
        resumed = BookSpeech(self.app, '/test/socket')
        with resumed.db() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM clips WHERE state='working'").fetchone()[0], 0)

    def test_failed_worker_backs_off_and_does_not_publish_partial_audio(self):
        identity = self.prepare()
        with patch('octofox_library.web_speech.UnixHTTP', side_effect=OSError('offline')):
            self.speech.render_one()
        first = self.speech.streams[identity]['plan'][0]
        self.assertIsNone(self.speech.ready(first))
        with self.speech.db() as db:
            row = db.execute('SELECT * FROM clips WHERE key=?', (first['key'],)).fetchone()
        self.assertEqual(row['state'], 'error')
        self.assertGreater(row['retry'], time.time())
        self.assertNotIn(b'Basic private', (self.speech.root / 'cache.sqlite3').read_bytes())

    def test_http_audio_auth_csrf_byte_ranges_and_asset(self):
        self.app.speech = self.speech
        self.app.sessions['test'] = self.session
        server = WebServer(('127.0.0.1', 0), self.app)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        origin = self.app.origin = f'http://127.0.0.1:{server.server_port}'
        identity = self.prepare()
        self.ready(self.speech.streams[identity]['plan'][0])
        path = f'/reader-api/speech/streams/{identity}/0.ts'
        def get(path, headers=None, body=None):
            return urlopen(Request(origin + path, headers=headers or {}, data=body), timeout=5)
        with patch.object(self.app, 'authenticate', return_value='alice'):
            with self.assertRaises(HTTPError) as caught:
                get(path)
            self.assertEqual(caught.exception.code, 401)
            headers = {'Cookie': 'books_session=test'}
            with get(path, {**headers, 'Range': 'bytes=2-4'}) as response:
                self.assertEqual(response.status, 206)
                self.assertEqual(response.headers['Content-Range'], 'bytes 2-4/5')
                self.assertEqual(response.read(), b'est')
            with get(path, {**headers, 'Range': 'bytes=-2'}) as response:
                self.assertEqual(response.read(), b'st')
            with self.assertRaises(HTTPError) as caught:
                get(path, {**headers, 'Range': 'bytes=9-'})
            self.assertEqual(caught.exception.code, 416)
            with self.assertRaises(HTTPError) as caught:
                get('/reader-api/books/42/speech', {**headers, 'Content-Type': 'application/json'}, b'{}')
            self.assertEqual(caught.exception.code, 403)
            with get('/server-voice.js') as response:
                self.assertIn(b'class ServerVoice', response.read())
                policy = response.headers['Content-Security-Policy']
                self.assertIn("media-src 'self' blob:", policy)
                self.assertIn("script-src 'self';", policy)


if __name__ == '__main__':
    unittest.main()
