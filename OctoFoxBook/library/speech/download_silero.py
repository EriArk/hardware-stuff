"""Download only the publisher's versioned Russian model, retain its provenance/license."""
import hashlib
import json
from pathlib import Path
import urllib.request

url = 'https://models.silero.ai/models/tts/ru/v5_5_ru.pt'
root = Path('/models')
target = root / 'silero-v5_5_ru.pt'
digest = hashlib.sha256()
size = 0
with urllib.request.urlopen(url, timeout=120) as source, target.open('wb') as output:
    while chunk := source.read(1024 * 1024):
        size += len(chunk)
        if size > 600 * 1024 * 1024:
            raise ValueError('Model size limit')
        digest.update(chunk)
        output.write(chunk)
if digest.hexdigest() != '50081637b602126ee06cb3bc8a744d25651d2da149ee8864b9a379bfdd934437':
    raise ValueError('Unexpected publisher model checksum')
(root / 'silero-manifest.json').write_text(json.dumps({'url': url, 'sha256': digest.hexdigest(), 'bytes': size}))
license_url = 'https://raw.githubusercontent.com/snakers4/silero-models/master/LICENSE'
(root / 'silero-LICENSE').write_bytes(urllib.request.urlopen(license_url, timeout=30).read(20000))
print('Russian Silero model:', size, digest.hexdigest(), flush=True)
