"""Wrap the relief continuously across the front fillet onto the sidewalls."""
from pathlib import Path
import json,math,time,shutil
import numpy as np
from scipy.interpolate import CubicSpline
import cadquery as cq
from OCP.BRepOffsetAPI import BRepOffsetAPI_ThruSections
from geometry_helpers import box,rr_xy,slot,union,cyl
R=Path(__file__).resolve().parents[1];O=R/'build/front-relief';O.mkdir(parents=True,exist_ok=True);T=time.time()
def log(s):print(f'{time.time()-T:.1f}s {s}',flush=True)
base=cq.Shape.importBrep(str(R/'source/base/front_before_relief.brep'))
paths=json.loads((R/'docs/relief_parameters.json').read_text())
def smooth(t):
 t=max(0,min(1,t));return t*t*(3-2*t)
def wrap(xy,side):
 """Developed distance maps to an exact R2.15 quarter-circle and sidewall."""
 r=2.15;edge=44.4 if side>0 else -52.4;flat=edge-side*r
 s=side*(xy[0]-flat)
 if s<=0:return np.array([xy[0],xy[1],0.]),np.array([0.,0.,-1.]),s
 if s<r*math.pi/2:
  a=s/r
  return np.array([flat+side*r*math.sin(a),xy[1],r*(1-math.cos(a))]),np.array([side*math.sin(a),0.,-math.cos(a)]),s
 return np.array([edge,xy[1],r+s-r*math.pi/2]),np.array([float(side),0.,0.]),s
ribbons=[];metadata=[]
for path in paths:
 name=path['name'];p=np.array(path['points'],float);side=1 if name=='upper_left' else -1
 ts=np.r_[0,np.cumsum(np.linalg.norm(np.diff(p,axis=0),axis=1))];ts/=ts[-1]
 spline=CubicSpline(ts,p,axis=0,bc_type='natural')
 def curve(t,der=0):return spline(0,1) if t<0 and der else (spline(0)+spline(0,1)*t if t<0 else spline(t,der))
 def point(t,u,bottom=False):
  c=curve(t);v=curve(t,1);v=v/np.linalg.norm(v);n=np.array([-v[1],v[0]])
  w=path['base_width']*(.13+.87*(1-t)**.65);xy=c+n*u*w/2
  pos,out,s=wrap(xy,side)
  fade_end=1-smooth((t-.86)/.14)
  # At the sidewall the root fades between Z2.15 and Z4.2. The front
  # and the entire quarter-circle keep the full intended relief height.
  fade_root=1-smooth((s-2.15*math.pi/2)/2.05)
  h=.9*(1-u*u)**2*fade_end*fade_root
  return pos+out*(-.8 if bottom else h-.005)
 start=-2.2/abs(curve(0,1)[0])
 def make_section(t):
  top=[cq.Vector(*point(t,u)) for u in np.linspace(-1,1,15)]
  bottom=[cq.Vector(*point(t,u,True)) for u in np.linspace(1,-1,15)]
  tangents=[cq.Vector(*(point(t,u+1e-5)-point(t,u-1e-5))) for u in [-1,1]]
  edges=[cq.Edge.makeSpline(top,tangents=tangents),cq.Edge.makeLine(top[-1],bottom[0]),cq.Edge.makeSpline(bottom),cq.Edge.makeLine(bottom[-1],top[0])]
  return cq.Wire.assembleEdges(edges)
 def make_loft(stations,ruled=False):
  loft=BRepOffsetAPI_ThruSections(True,ruled,1e-6);loft.CheckCompatibility(False);loft.SetMaxDegree(3)
  for t in stations:loft.AddWire(make_section(t).wrapped)
  loft.Build();s=cq.Shape.cast(loft.Shape());assert s.isValid(),name;return s
 stations=np.unique(np.r_[np.linspace(start,.25,55),np.linspace(.25,1,85)])
 ribbon=make_loft(stations,ruled=True)
 log(f'LOFT {name}')
 bowls=[]
 for t in np.linspace(.25,.77,path['suckers']):
  c=curve(t);v=curve(t,1);v=v/np.linalg.norm(v);n=np.array([-v[1],v[0]])
  w=path['base_width']*(.13+.87*(1-t)**.65);xy=c+n*w*.12
  pos,out,s=wrap(xy,side)
  h=.9*(1-.24**2)**2*(1-smooth((t-.86)/.14))*(1-smooth((s-2.15*math.pi/2)/2.05))-.005
  tangent=point(t+1e-5,.24)-point(t-1e-5,.24);tangent-=np.dot(tangent,out)*out;tangent/=np.linalg.norm(tangent)
  cross=np.cross(out,tangent)
  mat=np.eye(4);mat[:3,0]=tangent*.95;mat[:3,1]=cross*.64;mat[:3,2]=out*.42
  # Suckers lie on the flat front section, so keep the same simple
  # scale-then-rotate construction used in the previous revisions.
  ell=cq.Solid.makeSphere(1,angleDegrees1=-90).transformGeometry(cq.Matrix([[.95,0,0,0],[0,.64,0,0],[0,0,.42,0],[0,0,0,1]]))
  ell=ell.rotate((0,0,0),(0,0,1),math.degrees(math.atan2(tangent[1],tangent[0]))).translate(tuple(pos+out*(h+.24)))
  bowls.append(ell)
 if bowls:ribbon=ribbon.cut(cq.Compound.makeCompound(bowls)).clean()
 log(f'BOWLS {name}')
 ribbon.exportBrep(str(O/f'{name}_flat.brep'))
 # Join each overlapping piece directly to the case; this avoids a
 # standalone Boolean on almost tangent decorative surfaces.
 ribbons.append(ribbon);metadata.append({**path,'side':side,'side_fade_end_z':4.2})
 assert ribbon.isValid() and len(ribbon.Solids())==1,name
 ribbon.exportBrep(str(O/f'{name}_relief.brep'))
 log(f'RELIEF {name}')
keepouts=[rr_xy(5.5,2.66,65.8,112,2,-3,7),cyl(8.5,7,(-39.58,-57,-3))]
for cy in [5,-13.5]:keepouts.append(slot(-39.58,cy,17.8,10,-3,7))
front=base
for i,ribbon in enumerate(ribbons):
 log(f'JOIN {i}')
 piece=ribbon.cut(union(keepouts))
 front=front.fuse(piece)
 log(f'JOIN_RESULT {i}: valid={front.isValid()}, solids={len(front.Solids())}, faces={len(front.Faces())}')
 assert front.isValid() and len(front.Solids())==1,('join',i)
 front.exportBrep(str(O/f'join_{i}.brep'))
front=front.clean()
assert front.isValid() and len(front.Solids())==1
assert front.Volume(1e-5)>base.Volume(1e-5)+120,'Relief lost'
assert base.cut(front).Volume()<1e-5,'Unexpected removal of base geometry'
log('FRONT fused')
front.exportBrep(str(O/'t5_reader_case_v65_front.brep'));cq.exporters.export(front,str(O/'t5_reader_case_v65_front.step'))
front.exportStl(str(O/'t5_reader_case_v65_front.stl'),tolerance=.012,angularTolerance=.08,relative=False)

log('FRONT_RELIEF_BUILD_DONE')
