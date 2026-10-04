"""Blender 4.5: true CAD assembly, PBR finish, e-ink texture and studio."""
import bpy, numpy as np, json, sys, math
from pathlib import Path
from mathutils import Vector, Matrix

R=Path(__file__).resolve().parents[1]
O=Path(sys.argv[sys.argv.index('--scene-dir')+1]).resolve() if '--scene-dir' in sys.argv else R/'deliverables/AbyssBook_scene'
T=O/'textures'
QUICK='--quick' in sys.argv
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
scene=bpy.context.scene
scene.unit_settings.system='METRIC';scene.unit_settings.length_unit='MILLIMETERS'
model=bpy.data.collections.new('AbyssBook | assembled CAD');scene.collection.children.link(model)
studio=bpy.data.collections.new('Studio | lights and cameras');scene.collection.children.link(studio)

def rgb(s):
    a=[int(s[i:i+2],16)/255 for i in (0,2,4)]
    return tuple(v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4 for v in a)+(1,)

def image_node(nodes,name,uv='PaintUV',data=False):
    tex=nodes.new('ShaderNodeTexImage');tex.image=bpy.data.images.load(str(T/name),check_existing=True)
    if data:tex.image.colorspace_settings.name='Non-Color'
    u=nodes.new('ShaderNodeUVMap');u.uv_map=uv
    return tex,u

def mat(name,color,rough=.48,texture=None,scales=False,metal=0):
    m=bpy.data.materials.new(name);m.diffuse_color=rgb(color);m.use_nodes=True
    n=m.node_tree.nodes;l=m.node_tree.links;p=n.get('Principled BSDF')
    p.inputs['Base Color'].default_value=rgb(color);p.inputs['Roughness'].default_value=rough;p.inputs['Metallic'].default_value=metal
    if texture:
      tx,uv=image_node(n,texture,'PaintUV');l.new(uv.outputs['UV'],tx.inputs['Vector']);l.new(tx.outputs['Color'],p.inputs['Base Color'])
    if scales:
      tx,uv=image_node(n,'dragon_scales_normal.png','ScaleUV',True);l.new(uv.outputs['UV'],tx.inputs['Vector'])
      normal=n.new('ShaderNodeNormalMap');normal.uv_map='ScaleUV';normal.inputs['Strength'].default_value=.32
      l.new(tx.outputs['Color'],normal.inputs['Color']);l.new(normal.outputs['Normal'],p.inputs['Normal'])
    return m

ivory=mat('Ivory | satin painted resin','EEECE0',.48)
front=mat('Ivory front | recess wash and micro scales','EEECE0',.48,'front_wash.png',True)
relief=mat('Tentacles | satin ivory with recess wash','EEECE0',.34,'front_wash.png')
back=mat('Ivory back | recess wash and micro scales','EEECE0',.48,'back_wash.png',True)
forest=mat('Forest green | painted stripe recesses','29433A',.38)
sage=mat('Sage | button caps','A7B8A4',.40)
inside=mat('Internal holders | ivory resin','D4D7CB',.65)
metal=mat('Screws | dark brushed steel','555F59',.29,metal=.8)
screenmat=mat('E-ink | printed page, no emission','DBDCD1',.88,'screen_page.png')
screenedge=mat('E-ink | display edge','C3C7BC',.82)

def mesh_obj(name,v,f,n=None):
    # CAD millimeters -> meters, front face up, button column on the right.
    p=v.copy();p[:,0]=-(v[:,0]+4)*.001;p[:,1]=(v[:,1]+1.35)*.001;p[:,2]=(23-v[:,2])*.001
    me=bpy.data.meshes.new(name);me.vertices.add(len(p));me.vertices.foreach_set('co',p.ravel())
    me.loops.add(f.size);me.loops.foreach_set('vertex_index',f.ravel())
    me.polygons.add(len(f));me.polygons.foreach_set('loop_start',np.arange(len(f),dtype=np.int32)*3)
    me.polygons.foreach_set('loop_total',np.full(len(f),3,np.int32));me.update()
    me.polygons.foreach_set('use_smooth',np.ones(len(f),bool))
    obj=bpy.data.objects.new(name,me);model.objects.link(obj)
    if n is not None:
      n=n.copy();n[:,0]*=-1;n[:,2]*=-1
      me.normals_split_custom_set_from_vertices(n.tolist())
    return obj

manifest=json.loads((O/'scene_manifest.json').read_text(encoding='utf-8'))
for spec in manifest:
    name=spec['name'];role=spec['role'];d=np.load(O/'assets'/(name+'.npz'))
    v=d['v'];f=d['f'];obj=mesh_obj(name,v,f,d['n']);obj['source']=spec['source'];obj['revision']=name
    obj['source_sha256']=spec['source_sha256'];obj['units']='Source mm, scene m';obj['geometry']='CAD tessellation; vertices unchanged except assembly transform'
    material={'front':front,'back':back,'sage':sage,'inside':inside,'screen':screenedge,'metal':metal,'forest':forest}[role]
    obj.data.materials.append(material)
    if role=='front':
      obj.data.materials.append(forest);obj.data.materials.append(relief)
      mi=d['mat'].copy();centers=v[f].mean(1)
      # Paint satin on the raised CAD relief, leaving the body softly matte.
      raised=(centers[:,2]<-.025)|((centers[:,2]<3)&((centers[:,0]<-52.41)|(centers[:,0]>44.41)))
      mi[(mi==0)&raised]=2;obj.data.polygons.foreach_set('material_index',mi)
    if role in ('front','back'):
      uv=np.stack([((44.4-v[:,0]) if role=='front' else (v[:,0]+52.4))/96.8,(v[:,1]+69.85)/137],1)
      layer=obj.data.uv_layers.new(name='PaintUV');layer.data.foreach_set('uv',uv[f.ravel()].astype(np.float32).ravel())
      # Box projection keeps physical texel size on side walls as well.
      tri=v[f];normal=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]);axis=np.argmax(abs(normal),axis=1)
      uvfaces=np.empty((len(f),3,2),np.float32)
      for ax in range(3):
        ids=axis==ax;vv=tri[ids]
        if ax==2:
          uvfaces[ids,:,0]=vv[:,:,0]*(-1 if role=='front' else 1)/.8;uvfaces[ids,:,1]=vv[:,:,1]/.96
        elif ax==0:
          uvfaces[ids,:,0]=vv[:,:,1]/.8;uvfaces[ids,:,1]=vv[:,:,2]/.96
        else:
          uvfaces[ids,:,0]=vv[:,:,0]/.8;uvfaces[ids,:,1]=vv[:,:,2]/.96
      layer=obj.data.uv_layers.new(name='ScaleUV');layer.data.foreach_set('uv',uvfaces.ravel())
      obj.data.uv_layers.active_index=0
    print('IMPORTED',name,len(f),flush=True)

# Plane is 0.01 mm in front of the existing appearance display surface.
x0,x1=-24.55,35.55;y0,y1=-50.04,55.36;z=2.019
v=np.array([[x1,y0,z],[x0,y0,z],[x0,y1,z],[x1,y1,z]],np.float32)
f=np.array([[0,1,2],[0,2,3]],np.int32)
page=mesh_obj('E-ink page | UV mapped readable Russian text',v,f)
page.data.materials.append(screenmat)
uv=np.array([[0,0],[1,0],[1,1],[0,1]],np.float32)
layer=page.data.uv_layers.new(name='PaintUV');layer.data.foreach_set('uv',uv[f.ravel()].ravel())
page['display_only']='Authored appearance plane; not part of printable geometry'

def move_to(obj,collection):
    for c in list(obj.users_collection):c.objects.unlink(obj)
    collection.objects.link(obj)

def point(obj,target):
    forward=(Vector(target)-obj.location).normalized()
    right=forward.cross(Vector((0,1,0))).normalized()
    up=right.cross(forward).normalized()
    obj.rotation_euler=Matrix((right,up,-forward)).transposed().to_euler()

def camera(name,loc,target,scale):
    data=bpy.data.cameras.new(name);obj=bpy.data.objects.new(name,data);studio.objects.link(obj)
    obj.location=loc;point(obj,target);data.type='ORTHO';data.ortho_scale=scale;data.lens=55;data.clip_start=.001;data.clip_end=100
    return obj

hero=camera('Camera | front hero',(.105,.125,.35),(0,0,.012),.192)
straight=camera('Camera | front inspection',(0,0,.4),(0,0,0),.167)
rear=camera('Camera | rear inspection',(-.09,.08,-.32),(0,0,.014),.18)
scene.camera=hero

def area(name,loc,power,size,color,target=(0,0,.01)):
    data=bpy.data.lights.new(name,'AREA');data.energy=power*.035;data.shape='DISK';data.size=size;data.color=color
    obj=bpy.data.objects.new(name,data);studio.objects.link(obj);obj.location=loc;point(obj,target)

# Powers sized for a 14 cm product, no emissive screen.
area('Key | large warm softbox',(-.20,.10,.31),8,.23,(1,.95,.88))
area('Fill | cool softbox',(.22,-.03,.20),4,.22,(.91,.96,1))
area('Rim | overhead strip',(.05,.24,.27),6,.16,(1,1,.98))
area('Rear fill',(-.15,.12,-.23),5,.20,(1,.96,.9))

bpy.ops.mesh.primitive_plane_add(size=200,location=(0,0,-.0008));ground=bpy.context.object;ground.name='Studio surface';move_to(ground,studio)
ground.data.materials.append(mat('Warm chalk studio surface','D5D0C4',.82))
world=bpy.data.worlds.new('Soft neutral studio');world.use_nodes=True;world.node_tree.nodes['Background'].inputs[0].default_value=(.72,.76,.78,1);world.node_tree.nodes['Background'].inputs[1].default_value=.18;scene.world=world
scene.render.engine='CYCLES';scene.cycles.samples=32 if QUICK else 96;scene.cycles.use_denoising=True
scene.cycles.max_bounces=6;scene.cycles.diffuse_bounces=3;scene.cycles.glossy_bounces=3
try:
    prefs=bpy.context.preferences.addons['cycles'].preferences;prefs.compute_device_type='CUDA';prefs.get_devices()
    gpu=False
    for dev in prefs.devices:
      dev.use=dev.type=='CUDA';gpu|=dev.use;print('CYCLES_DEVICE',dev.name,dev.type,dev.use,flush=True)
    if gpu:scene.cycles.device='GPU'
except Exception as e:print('GPU_FALLBACK',repr(e),flush=True)
scene.render.resolution_x=1000 if QUICK else 1600;scene.render.resolution_y=1250 if QUICK else 2000;scene.render.resolution_percentage=100
scene.render.image_settings.file_format='PNG';scene.render.film_transparent=False
scene.view_settings.view_transform='AgX';scene.view_settings.look='AgX - Medium High Contrast';scene.view_settings.exposure=1.05
scene.render.filepath=str(O/'AbyssBook_front.png')

# GLB includes assembled device only; portable PBR materials and image textures.
bpy.ops.object.select_all(action='DESELECT')
for obj in model.objects:obj.select_set(True)
bpy.context.view_layer.objects.active=page
if not QUICK:
    bpy.ops.export_scene.gltf(filepath=str(O/'AbyssBook.glb'),export_format='GLB',use_selection=True,export_yup=True,export_materials='EXPORT',export_extras=True,export_texcoords=True,export_normals=True,export_cameras=False,export_lights=False)
    print('GLB_EXPORTED',flush=True)

# Add localized studio AO after GLB export. Painted cavity tint is in portable textures.
for material in [front,back,ivory,sage,relief]:
    nd=material.node_tree.nodes;li=material.node_tree.links;p=nd.get('Principled BSDF');base=p.inputs['Base Color']
    old=base.links[0].from_socket if base.is_linked else None
    ao=nd.new('ShaderNodeAmbientOcclusion');ao.inputs['Distance'].default_value=.0010;ao.samples=8
    mix=nd.new('ShaderNodeMixRGB');mix.blend_type='MULTIPLY';mix.inputs[0].default_value=.30
    if old:li.new(old,mix.inputs[1])
    else:mix.inputs[1].default_value=base.default_value
    li.new(ao.outputs['Color'],mix.inputs[2]);li.new(mix.outputs[0],base)

for im in bpy.data.images:
    if im.source=='FILE':im.pack()
for screen in bpy.data.screens:
    for ar in screen.areas:
      if ar.type=='VIEW_3D':
        ar.spaces.active.region_3d.view_perspective='CAMERA';ar.spaces.active.shading.type='MATERIAL'
scene['project']='AbyssBook';scene['finish']='Ivory resin, dark forest stripes, sage caps, localized gray-green wash'
scene['microtexture']='Normal map from same scale function as printable STL; macro CAD geometry unchanged'
scene['assembly']='Front and Sleep cap v66; other printable parts v65; purchased caps/display are appearance references'
if not QUICK:bpy.ops.wm.save_as_mainfile(filepath=str(O/'AbyssBook.blend'))
bpy.ops.render.render(write_still=True)
if not QUICK:
    ground.hide_render=True;scene.camera=rear;scene.render.filepath=str(O/'AbyssBook_back.png');bpy.ops.render.render(write_still=True)
    scene.camera=straight;scene.render.filepath=str(O/'AbyssBook_front_straight.png');bpy.ops.render.render(write_still=True)
    ground.hide_render=False;scene.camera=hero;scene.render.filepath=str(O/'AbyssBook_front.png')
print('SCENE_READY',flush=True)
