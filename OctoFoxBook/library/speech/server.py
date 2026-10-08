"""Private, single-flight book narration worker; no outbound network or credentials."""
import io
import html
import csv
import json
import logging
import os
import socketserver
import subprocess
import threading
import tempfile
import wave
import zipfile
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler
from pathlib import Path

import onnxruntime
from piper import PiperVoice, SynthesisConfig
from piper.config import PiperConfig

voices = OrderedDict()
PIPER_VOICES = {'ruslan': 'ru', 'ljspeech': 'en', 'thorsten': 'de',
                'siwis': 'fr', 'davefx': 'es', 'faber': 'pt'}
gate = threading.Lock()


def voice_for(language):
    if language not in voices:
        # Bound resident model memory when a reader switches between languages.
        piper_models = [key for key in voices if key != 'silero']
        if len(piper_models) >= 2:
            del voices[piper_models[0]]
        options = onnxruntime.SessionOptions()
        options.intra_op_num_threads = options.inter_op_num_threads = 1
        voices[language] = PiperVoice(
            session=onnxruntime.InferenceSession('/models/' + language + '.onnx',
                sess_options=options, providers=['CPUExecutionProvider']),
            config=PiperConfig.from_dict(json.loads(Path('/models/' + language + '.onnx.json').read_text())),
        )
    voices.move_to_end(language)
    return voices[language]


def synthesize(parts, language):
    if language not in (*PIPER_VOICES, 'eugene', 'kseniya') or not isinstance(parts, list) or not 1 <= len(parts) <= 32:
        raise ValueError('Invalid narration request')
    if any(not isinstance(p, dict) or not isinstance(p.get('text'), str) for p in parts):
        raise ValueError('Invalid narration text')
    if not 1 <= sum(len(p['text']) for p in parts) <= 700:
        raise ValueError('Narration text too long')
    if language in PIPER_VOICES:
        voice = voice_for(PIPER_VOICES[language])
        rate = voice.config.sample_rate
    else:
        import torch
        if 'silero' not in voices:
            torch.set_num_threads(1)
            voices['silero'] = torch.package.PackageImporter('/models/silero-v5_5_ru.pt').load_pickle('tts_models', 'model')
            voices['silero'].to(torch.device('cpu'))
        voice = voices['silero']
        rate = 24000
    # Slower phoneme synthesis, not a pitch-shifted recording. Paragraph-aware pauses.
    config = SynthesisConfig(length_scale=1.16, noise_scale=0.45, noise_w_scale=0.55,
                             normalize_audio=False)
    raw = io.BytesIO()
    marks = []
    with wave.open(raw, 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        for part in parts:
            start = output.getnframes() / rate
            if language in PIPER_VOICES:
                for chunk in voice.synthesize(part['text'], syn_config=config):
                    output.writeframesraw(chunk.audio_int16_bytes)
            else:
                try:
                    with torch.inference_mode():
                        samples = voice.apply_tts(ssml_text='<speak><prosody rate="slow">' + html.escape(part['text']) +
                            '</prosody></speak>', speaker=language, sample_rate=rate, put_accent=True, put_yo=True)
                    output.writeframesraw((samples.clamp(-1, 1) * 32767).to(torch.int16).numpy().tobytes())
                except ValueError:
                    # Silero rejects some Latin names, ISBNs and OCR symbols. Keep
                    # the text intact and speak this part with the already-loaded
                    # Russian Piper voice instead of blocking the entire page.
                    import numpy as np
                    fallback = voice_for('ru')
                    pcm = b''.join(c.audio_int16_bytes for c in fallback.synthesize(part['text'], syn_config=config))
                    samples = np.frombuffer(pcm, dtype='<i2')
                    if not len(samples):
                        raise ValueError('Fallback returned no audio')
                    if fallback.config.sample_rate != rate:
                        count = round(len(samples) * rate / fallback.config.sample_rate)
                        samples = np.interp(np.arange(count) * fallback.config.sample_rate / rate,
                                            np.arange(len(samples)), samples).astype('<i2')
                    output.writeframesraw(samples.tobytes())
                    logging.info('Narration used Russian fallback for one part')
            if output.getnframes() / rate > 115:
                raise ValueError('Narration duration limit')
            pause = 0.38 if part.get('paragraph') else 0.16
            output.writeframesraw(b'\0\0' * int(pause * rate))
            marks.append({'start': start, 'end': output.getnframes() / rate})
        duration = output.getnframes() / rate
    wav = raw.getvalue()
    # Six-second native HLS segments avoid long playlist polling gaps after short chapters.
    # Synthesize natural phrases first; segmentation does not alter speech or insert pauses.
    with tempfile.TemporaryDirectory(prefix='book-audio-') as temp:
        listing = Path(temp) / 'segments.csv'
        subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
            '-threads', '1', '-i', 'pipe:0', '-vn', '-c:a', 'aac', '-b:a', '64k',
            '-f', 'segment', '-segment_time', '6', '-reset_timestamps', '0',
            '-segment_list', str(listing), '-segment_list_type', 'csv',
            str(Path(temp) / 'part%03d.ts')], input=wav, capture_output=True, timeout=30, check=True)
        encoded, slices = bytearray(), []
        for name, start, end in csv.reader(listing.read_text().splitlines()):
            part = (Path(temp) / Path(name).name).read_bytes()
            slices.append({'offset': len(encoded), 'bytes': len(part), 'duration': float(end) - float(start)})
            encoded.extend(part)
        duration = sum(p['duration'] for p in slices)
    result = io.BytesIO()
    with zipfile.ZipFile(result, 'w', compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('audio.wav', wav)
        archive.writestr('audio.ts', encoded)
        archive.writestr('meta.json', json.dumps({'duration': duration, 'marks': marks,
                                                'slices': slices, 'continuousSlices': True}))
    return result.getvalue()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, body=b'{}', mime='application/json'):
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.reply(200 if self.path == '/health' else 404)

    def do_POST(self):
        if self.path != '/synthesize':
            self.reply(404)
            return
        if not gate.acquire(False):
            self.reply(409)
            return
        try:
            self.connection.settimeout(60)
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 12000:
                self.reply(413)
                return
            body = json.loads(self.rfile.read(length))
            self.reply(200, synthesize(body.get('parts'), body.get('language')), 'application/zip')
        except Exception as error:
            # No book text in logs; enough context to distinguish worker failures.
            logging.warning('Narration failed: %s', type(error).__name__)
            self.reply(503)
        finally:
            gate.release()


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    request_queue_size = 4


if __name__ == '__main__':
    os.umask(0o077)
    socket_path = Path('/run/speech/speech.sock')
    # Exclusive flock is held by the container entrypoint before replacing its own socket.
    socket_path.unlink(missing_ok=True)
    voice_for('ru')
    with Server(str(socket_path), Handler) as server:
        print('Book narration worker ready', flush=True)
        server.serve_forever()
