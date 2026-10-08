import hashlib, json, urllib.request
from pathlib import Path
root = Path("/models"); root.mkdir(exist_ok=True)
manifest = {}
expected = json.loads(Path("/app/voices.json").read_text())
for dest, metadata in expected.items():
    if Path(dest).name != dest or metadata['bytes'] > 100 * 1024 * 1024:
        raise ValueError('Invalid model manifest')
    data = urllib.request.urlopen(metadata['source'], timeout=120).read(metadata['bytes'] + 1)
    if len(data) != metadata['bytes'] or hashlib.sha256(data).hexdigest() != metadata['sha256']:
        raise ValueError('Pinned voice checksum mismatch')
    (root / dest).write_bytes(data)
    manifest[dest] = metadata
(root / "manifest.json").write_text(json.dumps(manifest, indent=2))
