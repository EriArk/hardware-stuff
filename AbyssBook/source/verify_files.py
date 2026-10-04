"""Validate delivery checksums, without CAD packages. Python 3.11 or newer."""
from pathlib import Path
import hashlib,json

root=Path(__file__).resolve().parents[1]
manifest=json.loads((root/'SHA256.json').read_text(encoding='utf-8'))
for name,expected in manifest.items():
    path=root/name
    assert path.is_file(),f'Missing: {name}'
    with path.open('rb') as stream:
        prefix=stream.read(100)
        if prefix.startswith(b'version https://git-lfs.github.com/spec/v1'):
            raise SystemExit(f'Git LFS pointer: {name}. Run git lfs pull first.')
        stream.seek(0)
        assert hashlib.file_digest(stream,'sha256').hexdigest()==expected,f'Checksum mismatch: {name}'
parts=json.loads((root/'parts.json').read_text())
assert len(parts)==7
assert len(list((root/'STL/print-set').glob('*.stl')))==7
assert len(list((root/'STEP/parts').glob('*.step')))==7
records=json.loads((root/'docs/verification/print-set-stl.json').read_text())
for part in parts:
    rec=records[part['stl']]
    assert rec['sha256']==manifest[part['stl']]
    assert rec['boundary_or_nonmanifold_edges']==0 and rec['connected_regions']==1
    assert rec['zero_area_triangles']==0 and rec.get('self_intersecting_faces',0)==0
print(f'PASS: {len(manifest)} files verified; 7 print parts; revisions v66 + v65.')
