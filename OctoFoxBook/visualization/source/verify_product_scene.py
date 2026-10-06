"""Reopen both scene formats and verify portable geometry, materials and images."""
import bpy,json,sys
from pathlib import Path
from mathutils import Vector

R=Path(__file__).resolve().parents[1]
O=Path(sys.argv[sys.argv.index('--scene-dir')+1]).resolve() if '--scene-dir' in sys.argv else R/'deliverables/AbyssBook_scene'
manifest=json.loads((O/'scene_manifest.json').read_text(encoding='utf-8'))
bpy.ops.wm.open_mainfile(filepath=str(O/'AbyssBook.blend'))
report={'blend':{},'glb':{}}
collection=bpy.data.collections['AbyssBook | assembled CAD']
assert len(collection.objects)==13,len(collection.objects)
checked=[]
for part in manifest:
    ob=bpy.data.objects[part['name']]
    assert len(ob.data.polygons)==part['triangles'],part['name']
    lo,hi=part['bounds_mm']
    expected=[[-(hi[0]+4)*.001,(lo[1]+1.35)*.001,(23-hi[2])*.001],[-(lo[0]+4)*.001,(hi[1]+1.35)*.001,(23-lo[2])*.001]]
    corners=[ob.matrix_world@Vector(c) for c in ob.bound_box]
    actual=[[min(c[i] for c in corners) for i in range(3)],[max(c[i] for c in corners) for i in range(3)]]
    error=max(abs(actual[j][i]-expected[j][i]) for i in range(3) for j in range(2))
    assert error<1e-7,(part['name'],error)
    checked.append({'name':part['name'],'triangles':len(ob.data.polygons),'max_bounds_error_m':error})
files=[im for im in bpy.data.images if im.source=='FILE']
assert all(im.packed_file for im in files),[im.name for im in files if not im.packed_file]
assert bpy.data.materials['E-ink | printed page, no emission'].node_tree.nodes.get('Principled BSDF').inputs['Emission Strength'].default_value==0
report['blend']={'objects':len(collection.objects),'packed_images':[im.name for im in files],'parts':checked,'eink_emission':0,'passed':True}

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=str(O/'AbyssBook.glb'))
objects=[ob for ob in bpy.data.objects if ob.type=='MESH']
assert len(objects)==13,len(objects)
expected=sum(p['triangles'] for p in manifest)+2
triangles=sum(len(ob.data.polygons) for ob in objects)
assert triangles==expected,(triangles,expected)
assert not [ob for ob in bpy.data.objects if ob.type in ('CAMERA','LIGHT')]
screen=bpy.data.objects['E-ink page | UV mapped readable Russian text']
assert len(screen.data.uv_layers)>=1
tex=[n.image for n in screen.data.materials[0].node_tree.nodes if n.type=='TEX_IMAGE']
assert tex and tex[0].size[0]==1600 and tex[0].size[1]==2700
report['glb']={'objects':len(objects),'triangles':triangles,'images':[im.name for im in bpy.data.images],'screen_texture_size':list(tex[0].size),'passed':True}
report['passed']=True
(O/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print('SCENE_VERIFICATION_PASS',flush=True)
