import hashlib, json, urllib.request
from pathlib import Path
revision = "1162a9173d0ce503555aed757976b7a9912eae4c"
base = "https://huggingface.co/rhasspy/piper-voices/resolve/" + revision + "/"
root = Path("/models"); root.mkdir(exist_ok=True)
manifest = {}
expected = json.loads(Path("/app/voices.json").read_text())
for language, voice in [("ru", "ru_RU-ruslan-medium"), ("en", "en_US-ljspeech-medium")]:
    locale, name, quality = voice.split("-")
    prefix = language + "/" + locale + "/" + name + "/" + quality + "/"
    for filename, dest in [(voice + ".onnx", language + ".onnx"), (voice + ".onnx.json", language + ".onnx.json"), ("MODEL_CARD", language + "-MODEL_CARD")]:
        data = urllib.request.urlopen(base + prefix + filename, timeout=120).read(100 * 1024 * 1024)
        if len(data) != expected[dest]['bytes'] or hashlib.sha256(data).hexdigest() != expected[dest]['sha256']:
            raise ValueError('Pinned voice checksum mismatch')
        (root / dest).write_bytes(data)
        manifest[dest] = {"source": base + prefix + filename, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2))
