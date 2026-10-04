"""Assemble the seven current STEP parts without changing their printable files."""
from pathlib import Path
import argparse,json
import cadquery as cq

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path,default=root/'build/AbyssBook_assembly.step')
args=parser.parse_args()
parts=json.loads((root/'parts.json').read_text())
assert len(parts)==7
assembly=cq.Assembly(name='AbyssBook')
for part in parts:
    shape=cq.importers.importStep(str(root/part['step'])).val()
    assert shape.isValid() and len(shape.Solids())==1,part['part']
    if part['part']=='back':shape=shape.translate((0,0,18))
    assembly.add(shape,name=part['part'],color=cq.Color(.94,.94,.91))
args.output.parent.mkdir(parents=True,exist_ok=True)
assembly.export(str(args.output))
restored=cq.importers.importStep(str(args.output)).val()
assert restored.isValid() and len(restored.Solids())==7
print('ASSEMBLY_PASS',args.output)
