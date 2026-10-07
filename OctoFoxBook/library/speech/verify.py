"""Exercise the real Unix HTTP worker, both voices, PCM audio and HLS output."""
import io
import http.client
import json
import socket
import tempfile
import threading
import wave
import zipfile

import server


class UnixConnection(http.client.HTTPConnection):
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(90)
        self.sock.connect(self.host)


with tempfile.TemporaryDirectory() as tmp:
    address = tmp + "/speech.sock"
    worker = server.Server(address, server.Handler)
    threading.Thread(target=worker.serve_forever, daemon=True).start()
    try:
        for voice in ("ruslan", "eugene"):
            connection = UnixConnection(address)
            body = json.dumps({"language": voice, "parts": [
                {"text": "Вечерний свет ложился на страницы открытой книги.", "paragraph": True}
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
                transport = archive.read("audio.ts")
                assert transport and transport[0] == 0x47
                assert metadata["slices"] and metadata["marks"]
                assert metadata["continuousSlices"] is True
                print(json.dumps({"voice": voice, "duration": metadata["duration"],
                                  "pcm_bytes": len(frames), "transport_bytes": len(transport)}), flush=True)
        connection = UnixConnection(address)
        connection.request("GET", "/health")
        assert connection.getresponse().status == 200
        connection.close()
    finally:
        worker.shutdown()
        worker.server_close()
