"""Sleep-only correction, based on the verified v65 case."""
from pathlib import Path
import time,json
import cadquery as cq
from geometry_helpers import box,slot,cyl,rr_xz,union
R=Path(__file__).resolve().parents[1];O=R/'build/sleep';O.mkdir(parents=True,exist_ok=True);T=time.time();sx=-40.45;sz=9.
def log(*a):print(round(time.time()-T,1),*a,flush=True)
def capsule(l,w,y,h):return slot(0,0,l,w,0,h,0).rotate((0,0,0),(1,0,0),-90).translate((sx,y,sz))
def save(n,s):
 s=s.clean();assert s.isValid() and len(s.Solids())==1,n
 p=O/f't5_reader_case_v66_{n}';s.exportBrep(str(p.with_suffix('.brep')));cq.exporters.export(s,str(p.with_suffix('.step')))
 s.exportStl(str(p.with_suffix('.stl')),tolerance=.008,angularTolerance=.06,relative=False);log('EXPORTED',n);return s
front0=cq.Shape.importBrep(str(R/'build/front-relief/t5_reader_case_v65_front.brep'))
cutter=capsule(10.6,7.4,66.39,3.)
front=save('front',front0.cut(cutter))
cap=capsule(8.8,5.6,65.8,3.75)
cap=cq.Workplane(obj=cap).edges('>Y').fillet(.85).val()
cap=cq.Workplane(obj=cap).edges('<Y').fillet(.35).val()
lower=rr_xz(sx,sz,4.8,4.0,.25,65.5,1.35)
upper=box(sx-1.30,sx+1.30,66.84,67.35,sz-1.05,sz+1.05)
# A short lead-in at the small bore avoids a sharp corner during assembly.
entry=cq.Workplane('XY').rect(2.8,2.3).workplane(offset=.10).rect(2.6,2.1).loft().val().rotate((0,0,0),(1,0,0),-90).translate((sx,66.85,sz))
plain=cap.cut(lower,upper,entry).clean();plain.exportBrep(str(O/'cap_without_grip_ribs.brep'))
ribs=[]
for axis,position in [('x',1.525),('z',1.275)]:
 for sign in [-1,1]:
  x=sx+(sign*position if axis=='x' else 0);z=sz+(sign*position if axis=='z' else 0)
  cone=cq.Solid.makeCone(.225,.30,.10,cq.Vector(x,66.85,z),cq.Vector(0,1,0))
  ribs.append(cone.fuse(cyl(.30,.40,(x,66.95,z),(0,1,0))))
ribsolid=union(ribs);ribsolid.exportBrep(str(O/'grip_ribs_envelope.brep'))
cap=save('sleep_button_cap',plain.fuse(ribsolid).intersect(cap))
params={'base':'v65','only_changed_parts':['front','sleep_button_cap'],'window_mm':[10.6,7.4],'cap_mm':[8.8,5.6,3.75],'rest_front_y':69.55,'nominal_switch_tip_y':67.35,'cap_rear_y':65.8,'stroke_mm':1.8,'additional_straight_travel_check_mm':.2,'rest_protrusion_mm':2.4,'locked_protrusion_mm':1.4,'fully_pressed_protrusion_mm':.6,'socket_upper_clear_mm':[2.6,2.1],'socket_small_section_depth_mm':.5,'socket_nominal_stem_mm':[2.5,2.0],'grip_rib_interference_per_side_mm':.025,'grip_ribs':4,'socket_lower_guide_relief_mm':[4.8,4.0],'socket_lower_relief_end_y':66.85,'no_global_scaling':True,'texture_preserved_from_v65':True}
(O/'sleep_parameters.json').write_text(json.dumps(params,indent=2));log('BUILD_DONE')
