"""Exercise the real Unix HTTP worker, both voices, PCM audio and HLS output."""
import io
import http.client
import json
import socket
import tempfile
import threading
import wave
import zipfile
import os
import math
from array import array
from pathlib import Path

import server


class UnixConnection(http.client.HTTPConnection):
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(90)
        self.sock.connect(self.host)


SAMPLES = {
    'ruslan': 'Вечерний свет ложился на страницы открытой книги. За окном тихо шумел дождь.',
    'eugene': 'Вечерний свет ложился на страницы открытой книги. За окном тихо шумел дождь.',
    'ljspeech': 'The evening light fell across the pages of an open book. Outside, the rain whispered through the trees. She turned the page and continued reading.',
    'thorsten': 'Das warme Abendlicht fiel auf die Seiten des geöffneten Buches. Draußen rauschte der Regen leise durch die Bäume. Sie blätterte um und las weiter.',
    'siwis': 'La lumière du soir éclairait les pages du livre ouvert. Dehors, la pluie murmurait doucement dans les arbres. Elle tourna la page et continua sa lecture.',
    'davefx': 'La luz del atardecer iluminaba las páginas del libro abierto. Afuera, la lluvia susurraba entre los árboles. Ella pasó la página y siguió leyendo.',
    'faber': 'A luz do entardecer iluminava as páginas do livro aberto. Lá fora, a chuva sussurrava entre as árvores. Ela virou a página e continuou a leitura.',
}
output = Path(os.environ['VERIFY_OUTPUT']) if os.environ.get('VERIFY_OUTPUT') else None
if output:
    output.mkdir(parents=True, exist_ok=True)
results = []
with tempfile.TemporaryDirectory() as tmp:
    address = tmp + "/speech.sock"
    worker = server.Server(address, server.Handler)
    threading.Thread(target=worker.serve_forever, daemon=True).start()
    try:
        for voice, text in SAMPLES.items():
            connection = UnixConnection(address)
            body = json.dumps({"language": voice, "parts": [
                {"text": text, "paragraph": True}
            ]}, ensure_ascii=False).encode()
            connection.request("POST", "/synthesize", body, {"Content-Type": "application/json"})
            response = connection.getresponse()
            result = response.read()
            assert response.status == 200, (voice, response.status)
            connection.close()
            with zipfile.ZipFile(io.BytesIO(result)) as archive:
                metadata = json.loads(archive.read("meta.json"))
                with wave.open(io.BytesIO(archive.read("audio.wav"))) as audio:
                    frames = audio.readframes(audio.getnframes())
                    assert audio.getnframes() > audio.getframerate()
                    assert any(frames), "Silent PCM"
                    assert audio.getnchannels() == 1 and audio.getsampwidth() == 2
                    samples = array('h', frames)
                    rms = math.sqrt(sum(x*x for x in samples) / len(samples))
                    assert rms > 20, 'Near-silent PCM'
                transport = archive.read("audio.ts")
                assert transport and transport[0] == 0x47
                assert metadata["slices"] and metadata["marks"]
                assert metadata["continuousSlices"] is True
                assert abs(sum(s['bytes'] for s in metadata['slices']) - len(transport)) == 0
                result = {"voice": voice, "duration": metadata["duration"], "rms": round(rms, 2),
                          "pcm_bytes": len(frames), "transport_bytes": len(transport)}
                results.append(result)
                if output:
                    (output / (voice + '.wav')).write_bytes(archive.read('audio.wav'))
                    (output / (voice + '.txt')).write_text(text, encoding='utf-8')
                print(json.dumps(result), flush=True)
        connection = UnixConnection(address)
        connection.request("GET", "/health")
        assert connection.getresponse().status == 200
        connection.close()
        if output:
            (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    finally:
        worker.shutdown()
        worker.server_close()
