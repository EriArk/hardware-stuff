import math
import cadquery as cq

def box(x0,x1,y0,y1,z0,z1):return cq.Solid.makeBox(x1-x0,y1-y0,z1-z0,cq.Vector(x0,y0,z0))
def cyl(r,h,p,d=(0,0,1)):return cq.Solid.makeCylinder(r,h,cq.Vector(*p),cq.Vector(*d))
def union(shapes):
 return shapes[0].fuse(*shapes[1:]).clean() if len(shapes)>1 else shapes[0]
def rr_xy(cx,cy,l,w,r,z,h,angle=0):
 s=cq.Workplane('XY').box(l,w,h,centered=(True,True,False)).edges('|Z').fillet(r).val()
 return s.rotate((0,0,0),(0,0,1),angle).translate((cx,cy,z))
def rr_xz(cx,cz,l,w,r,y,h):
 # Local +Z maps to global +Y, local Y maps to -Z.
 return rr_xy(0,0,l,w,r,0,h).rotate((0,0,0),(1,0,0),-90).translate((cx,y,cz))
def slot(cx,cy,l,w,z,h,angle=45):
 return cq.Workplane('XY',origin=(cx,cy,z)).slot2D(l,w,angle).extrude(h).val()
