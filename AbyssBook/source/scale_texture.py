"""Smooth, staggered dragon-scale relief and printable material coupons."""
from pathlib import Path
import numpy as np
import vtk
from PIL import Image,ImageDraw
from scipy.ndimage import distance_transform_edt,map_coordinates
from vtk.util.numpy_support import numpy_to_vtk,numpy_to_vtkIdTypeArray
R=Path(__file__).resolve().parents[1];O=R/'build';O.mkdir(parents=True,exist_ok=True)
_tile=None
def scales(x,y,pitch=.8,height=.10):
 """Periodic Euclidean distance to staggered scallops, with broad round shoulders."""
 global _tile
 if _tile is None:
  res=500;w=500;h=600
  im=Image.new('L',(3*w,3*h),255);draw=ImageDraw.Draw(im)
  u=np.linspace(-1,1,2001);cy=.6*(1-np.abs(u)**1.5)**.72
  for j in range(-2,8):
   for k in range(-1,5):
    xx=(u*.5+k+(j%2)*.5)*res;yy=(cy+j*.6)*res
    draw.line(list(zip(xx,yy)),fill=0,width=1)
  d=distance_transform_edt(np.array(im)>0)[h:2*h,w:2*w]/res
  _tile=1-np.exp(-(d/.16)**2);_tile/=_tile.max()
 x=np.asarray(x);y=np.asarray(y)
 coords=np.stack([(y.ravel()/pitch/.6/2%1)*600,(x.ravel()/pitch%1)*500])
 return height*map_coordinates(_tile,coords,order=1,mode='grid-wrap').reshape(x.shape)
def polydata(v,f):
 p=vtk.vtkPolyData();points=vtk.vtkPoints();points.SetData(numpy_to_vtk(np.asarray(v,np.float64),deep=True));p.SetPoints(points)
 cells=vtk.vtkCellArray();off=np.arange(0,3*(len(f)+1),3,dtype=np.int64)
 cells.SetData(numpy_to_vtkIdTypeArray(off,deep=True),numpy_to_vtkIdTypeArray(np.asarray(f,np.int64).ravel(),deep=True));p.SetPolys(cells);return p
def save_stl(path,v,f):
 out=vtk.vtkSTLWriter();out.SetFileName(str(path));out.SetFileTypeToBinary();out.SetInputData(polydata(v,f));assert out.Write()==1
def coupon(pitch,height):
 dx=.04;nx=651;ny=551
 xs=np.linspace(-13,13,nx);ys=np.linspace(-11,11,ny);xx,yy=np.meshgrid(xs,ys)
 edge=np.minimum(12-np.abs(xx),10-np.abs(yy));fade=np.clip(edge/.8,0,1);fade=fade*fade*(3-2*fade)
 z=2+scales(xx.ravel(),yy.ravel(),pitch,height).reshape(xx.shape)*fade
 top=np.stack([xx.ravel(),yy.ravel(),z.ravel()],axis=1)
 a=np.arange((ny-1)*nx).reshape(ny-1,nx)[:,:-1].ravel();b=a+1;c=a+nx;d=c+1
 f=np.vstack([np.stack([a,b,d],1),np.stack([a,d,c],1)])
 border=np.r_[np.arange(nx),np.arange(2*nx-1,nx*ny,nx),np.arange(nx*ny-2,nx*(ny-1)-1,-1),np.arange(nx*(ny-2),0,-nx)]
 # Boundary order is counterclockwise when viewed from above.
 vb=top[border].copy();vb[:,2]=0;base=np.arange(len(vb))+len(top)
 v=np.vstack([top,vb,[[0,0,0]]]);center=len(v)-1
 q=np.roll(border,-1);bn=np.roll(base,-1)
 f=np.vstack([f,np.stack([border,base,bn],1),np.stack([border,bn,q],1),np.stack([np.full(len(base),center),bn,base],1)])
 return v,f
if __name__=='__main__':
 import json
 T=O/'texture_samples';T.mkdir(exist_ok=True)
 specs=[]
 for i,(pitch,h) in enumerate([(.6,.06),(.8,.10),(1.,.14)],1):
  v,f=coupon(pitch,h);name=f'scale_sample_{i}_{pitch:.1f}mm_{h:.2f}mm'
  save_stl(T/(name+'.stl'),v,f)
  specs.append({'file':name+'.stl','scale_pitch_mm':pitch,'relief_height_mm':h,'coupon_size_mm':[26,22,2]})
  print('COUPON',name,len(f),flush=True)
 (T/'parameters.json').write_text(json.dumps(specs,indent=2))
 print('SCALE_SAMPLES_DONE',flush=True)
