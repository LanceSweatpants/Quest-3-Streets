# Quest 3 Street: the Vegas basketball plaza that surrounds the big screen in VR (menus, loading
# screens, and courts without their own 360). Lance 2026-10-14: "make it look like a vegas type
# basketball theme with flickering lights ... like disney/pixar animations". Original art, built
# from code in Blender (no AI, no NBA or team logos).
#
# Run headless:  blender -b --factory-startup --python vegas_scene.py -- <variant> <mode> <out_dir>
#   variant: night | dusk
#   mode:    preview  -> one equirectangular render (look-dev)
#            faces    -> the six cube faces (with overscan for the glow), see vegas_post.py
# Every light belongs to a Cycles light group, written as its own EXR pass, so the app can switch
# groups on and off (marquee chasers, flickering neon, LED boards):
#   base, chase1, chase2, chase3, flicker, boardsA, boardsB
# Blender: X right, Y forward (where the big screen hangs), Z up. The viewer stands at centre court,
# eyes at 1.6 m.

import bpy
import bmesh
import math
import os
import random
import sys

import numpy as np
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
VARIANT = args[0] if len(args) > 0 else "night"
MODE = args[1] if len(args) > 1 else "preview"
OUT = args[2] if len(args) > 2 else "C:/StreetMR/tools/vegas/out"
SAMPLES = int(args[3]) if len(args) > 3 else (48 if MODE == "preview" else 128)
FACE = int(args[4]) if len(args) > 4 else 1536
OVERSCAN = 1.15  # faces are rendered wider, the glow computed, then cropped back to 90 degrees
EYE = Vector((0.0, 0.0, 1.6))
GROUPS = ["base", "chase1", "chase2", "chase3", "flicker", "boardsA", "boardsB"]

random.seed(7)
os.makedirs(OUT, exist_ok=True)

# ------------------------------------------------------------------------------------------------
# Scene setup
# ------------------------------------------------------------------------------------------------
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = SAMPLES
scene.cycles.use_adaptive_sampling = True
scene.cycles.adaptive_threshold = 0.02
scene.cycles.use_denoising = False  # denoised per pass in the compositor
scene.cycles.max_bounces = 6
scene.cycles.diffuse_bounces = 2
scene.cycles.glossy_bounces = 2
scene.cycles.transmission_bounces = 4
scene.cycles.transparent_max_bounces = 8
scene.cycles.caustics_reflective = False
scene.cycles.caustics_refractive = False
scene.cycles.sample_clamp_indirect = 6.0
scene.cycles.use_light_tree = True
scene.render.film_transparent = False
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.render.use_persistent_data = True
vl = scene.view_layers[0]
vl.use_pass_mist = True
vl.cycles.denoising_store_passes = True
for g in GROUPS:
    vl.lightgroups.add(name=g)

world = bpy.data.worlds.new("World")
scene.world = world
world.lightgroup = "base"
world.mist_settings.start = 70.0
world.mist_settings.depth = 900.0
world.mist_settings.falloff = "QUADRATIC"


def mat(name, color, rough=0.5, metal=0.0, emit=None, emit_strength=0.0, coat=0.0, alpha=1.0,
        transmission=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (*color, 1.0)
    p.inputs["Roughness"].default_value = rough
    p.inputs["Metallic"].default_value = metal
    p.inputs["Coat Weight"].default_value = coat
    p.inputs["Alpha"].default_value = alpha
    p.inputs["Transmission Weight"].default_value = transmission
    if emit is not None:
        p.inputs["Emission Color"].default_value = (*emit, 1.0)
        p.inputs["Emission Strength"].default_value = emit_strength
    return m


def emissive(name, color, strength):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    e = nt.nodes.new("ShaderNodeEmission")
    e.inputs["Color"].default_value = (*color, 1.0)
    e.inputs["Strength"].default_value = strength
    nt.links.new(e.outputs[0], out.inputs[0])
    return m


def ghost_emissive(name, color, strength):
    """Glow with nothing behind it: see-through unless its own light group is on (the LED words
    A and B share a spot; unlit, neither may hide the other or the screen)."""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    e = nt.nodes.new("ShaderNodeEmission")
    e.inputs["Color"].default_value = (*color, 1.0)
    e.inputs["Strength"].default_value = strength
    tr = nt.nodes.new("ShaderNodeBsdfTransparent")
    add = nt.nodes.new("ShaderNodeAddShader")
    nt.links.new(e.outputs[0], add.inputs[0])
    nt.links.new(tr.outputs[0], add.inputs[1])
    nt.links.new(add.outputs[0], out.inputs[0])
    return m


def link(obj, coll=None):
    (coll or scene.collection).objects.link(obj)
    return obj


def box(name, size, loc, material, bevel=0.0, rot_z=0.0, group="base"):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    bm.to_mesh(me)
    bm.free()
    ob = link(bpy.data.objects.new(name, me))
    ob.location = loc
    ob.rotation_euler = (0, 0, rot_z)
    ob.data.materials.append(material)
    if bevel > 0:
        b = ob.modifiers.new("bevel", "BEVEL")
        b.width = bevel
        b.segments = 3
        b.limit_method = "ANGLE"
        b.harden_normals = True
        for poly in me.polygons:
            poly.use_smooth = True
        ob.modifiers.new("wn", "WEIGHTED_NORMAL").keep_sharp = True
    ob.lightgroup = group
    return ob


def cylinder(name, r, depth, loc, material, verts=32, rot=(0, 0, 0), group="base", r2=None):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=verts, radius1=r,
                          radius2=r if r2 is None else r2, depth=depth)
    bm.to_mesh(me)
    bm.free()
    for poly in me.polygons:
        poly.use_smooth = True
    ob = link(bpy.data.objects.new(name, me))
    ob.location = loc
    ob.rotation_euler = rot
    ob.data.materials.append(material)
    ob.lightgroup = group
    return ob


def sphere(name, r, loc, material, group="base", subdiv=4):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=r)
    bm.to_mesh(me)
    bm.free()
    for poly in me.polygons:
        poly.use_smooth = True
    ob = link(bpy.data.objects.new(name, me))
    ob.location = loc
    ob.data.materials.append(material)
    ob.lightgroup = group
    return ob


def bulbs(name, points, r, material, group):
    """Many small bulbs as one object (one light group)."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    for p in points:
        bmesh.ops.create_icosphere(bm, subdivisions=1, radius=r, matrix=Matrix.Translation(p))
    bm.to_mesh(me)
    bm.free()
    for poly in me.polygons:
        poly.use_smooth = True
    ob = link(bpy.data.objects.new(name, me))
    ob.data.materials.append(material)
    ob.lightgroup = group
    return ob


def text(name, body, loc, size, material, rot=(math.radians(90), 0, 0), extrude=0.02,
         font_file="C:/Windows/Fonts/impact.ttf", align="CENTER", group="base"):
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = body
    cu.size = size
    cu.extrude = extrude
    cu.align_x = align
    cu.align_y = "CENTER"
    try:
        cu.font = bpy.data.fonts.load(font_file, check_existing=True)
    except Exception:
        pass
    ob = link(bpy.data.objects.new(name, cu))
    ob.location = loc
    ob.rotation_euler = rot
    ob.data.materials.append(material)
    ob.lightgroup = group
    return ob


def face_towards_eye(ob, extra=0.0):
    """Turns an upright object (front = -Y local) to face the viewer."""
    d = Vector((EYE.x - ob.location.x, EYE.y - ob.location.y, 0.0))
    ob.rotation_euler[2] = math.atan2(d.x, -d.y) + extra


def rect_points(cx, cz, w, h, spacing):
    """Points around a rectangle in a local XZ plane (y = 0), clockwise from top-left."""
    pts = []
    n_w = max(2, int(w / spacing))
    n_h = max(2, int(h / spacing))
    for i in range(n_w):
        pts.append((cx - w / 2 + w * i / n_w, cz + h / 2))
    for i in range(n_h):
        pts.append((cx + w / 2, cz + h / 2 - h * i / n_h))
    for i in range(n_w):
        pts.append((cx + w / 2 - w * i / n_w, cz - h / 2))
    for i in range(n_h):
        pts.append((cx - w / 2, cz - h / 2 + h * i / n_h))
    return pts


# ------------------------------------------------------------------------------------------------
# Materials
# ------------------------------------------------------------------------------------------------
NIGHT = VARIANT == "night"
asphalt = mat("asphalt", (0.09, 0.085, 0.11), rough=0.5, coat=0.3)
concrete = mat("concrete", (0.32, 0.30, 0.34), rough=0.7)
white_paint = mat("white", (0.85, 0.85, 0.88), rough=0.35, coat=0.3)
black_gloss = mat("black", (0.02, 0.02, 0.025), rough=0.25, coat=0.6)
orange_pad = mat("orange_pad", (0.95, 0.30, 0.04), rough=0.4, coat=0.4)
rim_orange = mat("rim", (1.0, 0.25, 0.02), rough=0.25, metal=0.6)
chrome = mat("chrome", (0.9, 0.9, 0.95), rough=0.12, metal=1.0)
gold = mat("gold", (1.0, 0.72, 0.25), rough=0.2, metal=1.0)
glass = mat("glass", (0.92, 0.97, 1.0), rough=0.03, transmission=1.0)
seat_teal = mat("seat_teal", (0.02, 0.55, 0.62), rough=0.3, coat=0.5)
seat_magenta = mat("seat_magenta", (0.75, 0.05, 0.42), rough=0.3, coat=0.5)
seat_gold = mat("seat_gold", (0.95, 0.62, 0.05), rough=0.3, coat=0.5)
palm_trunk = mat("trunk", (0.28, 0.16, 0.08), rough=0.8)
palm_leaf = mat("leaf", (0.08, 0.42, 0.12), rough=0.55)
bulb_off = mat("bulb_off", (0.85, 0.78, 0.6), rough=0.2, emit=(1.0, 0.8, 0.45), emit_strength=0.25)
# A chaser bulb: pale glass under the other lights, its glow only in its own light group.
bulb_on = mat("bulb_on", (0.9, 0.82, 0.62), rough=0.15, coat=0.8, emit=(1.0, 0.78, 0.42),
              emit_strength=28.0)
led_dark = mat("led_dark", (0.01, 0.01, 0.015), rough=0.3, coat=0.5)

NEON = {
    "pink": (1.0, 0.12, 0.55),
    "cyan": (0.1, 0.85, 1.0),
    "gold": (1.0, 0.70, 0.12),
    "red": (1.0, 0.08, 0.06),
    "green": (0.2, 1.0, 0.35),
    "purple": (0.62, 0.2, 1.0),
    "orange": (1.0, 0.42, 0.05),
    "white": (1.0, 0.95, 0.9),
}
neon = {k: emissive("neon_" + k, v, 18.0) for k, v in NEON.items()}
neon_soft = {k: emissive("neon_soft_" + k, v, 6.0) for k, v in NEON.items()}
ghost = {k: ghost_emissive("ghost_" + k, v, 14.0) for k, v in NEON.items()}
# Neon that switches (flicker group): a pale glass tube when off.
neon_tube = {k: mat("neon_tube_" + k, tuple(0.35 + 0.4 * c for c in v), rough=0.15, coat=0.8,
                    emit=v, emit_strength=18.0) for k, v in NEON.items()}

# ------------------------------------------------------------------------------------------------
# The sky
# ------------------------------------------------------------------------------------------------
wt = world.node_tree if world.use_nodes else None
world.use_nodes = True
wt = world.node_tree
wt.nodes.clear()
wout = wt.nodes.new("ShaderNodeOutputWorld")
bg = wt.nodes.new("ShaderNodeBackground")
texco = wt.nodes.new("ShaderNodeTexCoord")
sep = wt.nodes.new("ShaderNodeSeparateXYZ")
wt.links.new(texco.outputs["Generated"], sep.inputs[0])
# Height of the view direction (z) through a colour ramp: horizon glow to deep sky.
ramp = wt.nodes.new("ShaderNodeValToRGB")
mapz = wt.nodes.new("ShaderNodeMapRange")
mapz.inputs["From Min"].default_value = -0.05
mapz.inputs["From Max"].default_value = 0.75
wt.links.new(sep.outputs["Z"], mapz.inputs["Value"])
wt.links.new(mapz.outputs["Result"], ramp.inputs["Fac"])
cr = ramp.color_ramp
cr.interpolation = "B_SPLINE"
if NIGHT:
    stops = [(0.0, (0.55, 0.18, 0.30)), (0.07, (0.38, 0.10, 0.36)), (0.22, (0.12, 0.04, 0.22)),
             (0.55, (0.025, 0.015, 0.08)), (1.0, (0.008, 0.006, 0.03))]
else:
    stops = [(0.0, (1.6, 0.62, 0.18)), (0.08, (1.2, 0.38, 0.22)), (0.18, (0.62, 0.16, 0.34)),
             (0.38, (0.18, 0.07, 0.30)), (0.7, (0.05, 0.04, 0.20)), (1.0, (0.02, 0.025, 0.12))]
cr.elements[0].position, cr.elements[0].color = stops[0][0], (*stops[0][1], 1)
cr.elements[1].position, cr.elements[1].color = stops[-1][0], (*stops[-1][1], 1)
for pos, col in stops[1:-1]:
    e = cr.elements.new(pos)
    e.color = (*col, 1)
# Stars: sharp noise above the horizon (night: many, dusk: a few up high).
noise = wt.nodes.new("ShaderNodeTexVoronoi")
noise.feature = "F1"
noise.inputs["Scale"].default_value = 420.0
smax = wt.nodes.new("ShaderNodeMath")
smax.operation = "LESS_THAN"
smax.inputs[1].default_value = 0.035 if NIGHT else 0.02
wt.links.new(texco.outputs["Generated"], noise.inputs["Vector"])
wt.links.new(noise.outputs["Distance"], smax.inputs[0])
above = wt.nodes.new("ShaderNodeMath")
above.operation = "GREATER_THAN"
above.inputs[1].default_value = 0.25 if NIGHT else 0.5
wt.links.new(sep.outputs["Z"], above.inputs[0])
starmask = wt.nodes.new("ShaderNodeMath")
starmask.operation = "MULTIPLY"
wt.links.new(smax.outputs[0], starmask.inputs[0])
wt.links.new(above.outputs[0], starmask.inputs[1])
starscale = wt.nodes.new("ShaderNodeMath")
starscale.operation = "MULTIPLY"
starscale.inputs[1].default_value = 2.5 if NIGHT else 0.8
wt.links.new(starmask.outputs[0], starscale.inputs[0])
addstars = wt.nodes.new("ShaderNodeMix")
addstars.data_type = "RGBA"
addstars.blend_type = "ADD"
addstars.inputs["Factor"].default_value = 1.0
wt.links.new(ramp.outputs["Color"], addstars.inputs[6])
comb = wt.nodes.new("ShaderNodeCombineColor")
wt.links.new(starscale.outputs[0], comb.inputs[0])
wt.links.new(starscale.outputs[0], comb.inputs[1])
wt.links.new(starscale.outputs[0], comb.inputs[2])
wt.links.new(comb.outputs[0], addstars.inputs[7])
wt.links.new(addstars.outputs[2], bg.inputs["Color"])
bg.inputs["Strength"].default_value = 1.0
wt.links.new(bg.outputs[0], wout.inputs[0])

# Moon (night) or the sun's last light (dusk).
if NIGHT:
    moon = sphere("moon", 30.0, (-700, 1400, 900), emissive("moon", (0.85, 0.88, 1.0), 6.0))
    sun = bpy.data.lights.new("moonlight", "SUN")
    sun.energy = 0.25
    sun.color = (0.6, 0.65, 1.0)
    sun.angle = math.radians(1.0)
    so = link(bpy.data.objects.new("moonlight", sun))
    so.rotation_euler = (math.radians(50), 0, math.radians(200))
    so.lightgroup = "base"
else:
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 1.4
    sun.color = (1.0, 0.55, 0.3)
    sun.angle = math.radians(3.0)
    so = link(bpy.data.objects.new("sun", sun))
    so.rotation_euler = (math.radians(84), 0, math.radians(150))
    so.lightgroup = "base"
    # the sun itself, half down behind the mountains (back left)
    a = math.radians(-120)
    sphere("sun_disc", 70.0, (math.sin(a) * 1900, math.cos(a) * 1900, 40.0),
           emissive("sun_disc", (1.0, 0.55, 0.2), 9.0))

# ------------------------------------------------------------------------------------------------
# The court (texture painted in numpy: indigo floor, magenta keys, gold lines, a ball at centre)
# ------------------------------------------------------------------------------------------------
CW, CH = 28.0, 15.0  # metres
TW, TH = 2800, 1500  # texels
yy, xx = np.mgrid[0:TH, 0:TW]
X = (xx + 0.5) / TW * CW - CW / 2
Y = (yy + 0.5) / TH * CH - CH / 2
img = np.zeros((TH, TW, 3), np.float32)
img[:] = (0.10, 0.06, 0.32)
# inside the three-point lines: a lighter indigo
for side in (-1, 1):
    bx = side * (CW / 2 - 1.575)
    d = np.sqrt((X - bx) ** 2 + Y ** 2)
    inside = (d < 7.24) & (np.abs(Y) < 6.6) & (side * X > side * bx - 7.3)
    corner = (np.abs(Y) < 6.6) & (side * X > CW / 2 - 4.2)
    img[inside | corner] = (0.16, 0.10, 0.45)
    key = (np.abs(Y) < 2.45) & (side * X > CW / 2 - 5.8)
    img[key] = (0.70, 0.06, 0.38)
# centre circle fill: an orange basketball
dc = np.sqrt(X ** 2 + Y ** 2)
ball = dc < 1.6
img[ball] = (1.0, 0.32, 0.03)
seam = ball & ((np.abs(X) < 0.035) | (np.abs(Y) < 0.035) |
               (np.abs(np.sqrt((X - 1.9) ** 2 + Y ** 2) - 1.45) < 0.035) |
               (np.abs(np.sqrt((X + 1.9) ** 2 + Y ** 2) - 1.45) < 0.035))
img[seam] = (0.05, 0.02, 0.02)
# lines (5 cm)
lw = 0.05
line = np.zeros((TH, TW), bool)
line |= (np.abs(np.abs(X) - (CW / 2 - lw)) < lw / 2) & (np.abs(Y) < CH / 2)
line |= (np.abs(np.abs(Y) - (CH / 2 - lw)) < lw / 2) & (np.abs(X) < CW / 2)
line |= np.abs(X) < lw / 2
line |= np.abs(dc - 1.8) < lw / 2
for side in (-1, 1):
    bx = side * (CW / 2 - 1.575)
    d = np.sqrt((X - bx) ** 2 + Y ** 2)
    arc = (np.abs(d - 7.24) < lw / 2) & (np.abs(Y) < 6.6) & (side * (X - bx) > -0.1)
    straight = (np.abs(np.abs(Y) - 6.6) < lw / 2) & (side * X > CW / 2 - 4.2)
    keyline = ((np.abs(np.abs(Y) - 2.45) < lw / 2) & (side * X > CW / 2 - 5.8)) | \
              ((np.abs(side * X - (CW / 2 - 5.8)) < lw / 2) & (np.abs(Y) < 2.45))
    ft = (np.abs(np.sqrt((X - side * (CW / 2 - 5.8)) ** 2 + Y ** 2) - 1.8) < lw / 2)
    line |= arc | straight | keyline | ft
img[line] = (1.0, 0.72, 0.18)
# The colours above are linear albedo. Written to a PNG and loaded back: Cycles renders a generated
# image's own (blank) content, not pixels set from Python.
def write_png(path, rgb01):
    import struct
    import zlib
    data = (np.clip(rgb01, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w = data.shape[:2]
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


srgb_img = np.where(img <= 0.0031308, 12.92 * img, 1.055 * np.power(np.clip(img, 0, 1), 1 / 2.4) - 0.055)
court_png = os.path.join(OUT, "court_texture.png")
write_png(court_png, srgb_img)  # row 0 = the top of the picture, as PNG wants
court_img = bpy.data.images.load(court_png)
court_img.colorspace_settings.name = "sRGB"

court_mat = bpy.data.materials.new("court")
court_mat.use_nodes = True
nt = court_mat.node_tree
p = nt.nodes["Principled BSDF"]
tex = nt.nodes.new("ShaderNodeTexImage")
tex.image = court_img
nt.links.new(tex.outputs["Color"], p.inputs["Base Color"])
p.inputs["Roughness"].default_value = 0.16
p.inputs["Coat Weight"].default_value = 0.8
p.inputs["Coat Roughness"].default_value = 0.05
bpy.ops.mesh.primitive_plane_add(size=1.0, location=(0, 0, 0.02))
court = bpy.context.active_object
court.scale = (CW, CH, 1)
court.data.materials.append(court_mat)
court.lightgroup = "base"
# apron around the court and the plaza beyond
# (its top 2 cm under the court: coincident faces block each other's light rays)
box("apron", (CW + 6, CH + 6, 0.04), (0, 0, -0.02), mat("apron", (0.30, 0.05, 0.25), rough=0.3, coat=0.5))
bpy.ops.mesh.primitive_plane_add(size=3000.0, location=(0, 0, -0.01))
ground = bpy.context.active_object
ground.data.materials.append(asphalt)
ground.lightgroup = "base"

# ------------------------------------------------------------------------------------------------
# Hoops (left and right: the court runs across the view)
# ------------------------------------------------------------------------------------------------
net_mat = bpy.data.materials.new("net")
net_mat.use_nodes = True
nt = net_mat.node_tree
nodes = nt.nodes
pn = nodes["Principled BSDF"]
pn.inputs["Base Color"].default_value = (0.95, 0.95, 0.95, 1)
brick = nodes.new("ShaderNodeTexBrick")
brick.inputs["Scale"].default_value = 18.0
brick.inputs["Mortar Size"].default_value = 0.04
brick.offset = 0.5
# the net's mesh: around (angle) and down (z), from object coordinates (no UVs needed)
tc = nodes.new("ShaderNodeTexCoord")
nsep = nodes.new("ShaderNodeSeparateXYZ")
nt.links.new(tc.outputs["Object"], nsep.inputs[0])
nang = nodes.new("ShaderNodeMath")
nang.operation = "ARCTAN2"
nt.links.new(nsep.outputs["Y"], nang.inputs[0])
nt.links.new(nsep.outputs["X"], nang.inputs[1])
nscale = nodes.new("ShaderNodeMath")
nscale.operation = "MULTIPLY"
nscale.inputs[1].default_value = 0.16
nt.links.new(nang.outputs[0], nscale.inputs[0])
ncomb = nodes.new("ShaderNodeCombineXYZ")
nt.links.new(nscale.outputs[0], ncomb.inputs["X"])
nt.links.new(nsep.outputs["Z"], ncomb.inputs["Y"])
nt.links.new(ncomb.outputs[0], brick.inputs["Vector"])
inv = nodes.new("ShaderNodeMath")
inv.operation = "SUBTRACT"
inv.inputs[0].default_value = 1.0
nt.links.new(brick.outputs["Fac"], inv.inputs[1])
transp = nodes.new("ShaderNodeBsdfTransparent")
mix = nodes.new("ShaderNodeMixShader")
nt.links.new(inv.outputs[0], mix.inputs[0])
nt.links.new(pn.outputs[0], mix.inputs[1])
nt.links.new(transp.outputs[0], mix.inputs[2])
nt.links.new(mix.outputs[0], nodes["Material Output"].inputs[0])

for side in (-1, 1):
    rim_x = side * (CW / 2 - 1.575)
    board_x = side * (CW / 2 - 1.2)
    # glass backboard with a white frame and a glowing LED edge
    box(f"board{side}", (0.03, 1.8, 1.05), (board_x, 0, 3.4), glass, group="base")
    for dz, h in ((0.5, 0.06), (-0.5, 0.06)):
        box(f"frame_h{side}{dz}", (0.06, 1.86, h), (board_x, 0, 3.4 + dz), white_paint)
    for dy in (-0.9, 0.9):
        box(f"frame_v{side}{dy}", (0.06, 0.06, 1.06), (board_x, dy, 3.4), white_paint)
    box(f"led{side}", (0.035, 1.9, 0.03), (board_x, 0, 2.85), neon["cyan"], group="base")
    box(f"square{side}", (0.04, 0.59, 0.45), (board_x - side * 0.02, 0, 3.27), white_paint)
    box(f"square_in{side}", (0.045, 0.53, 0.39), (board_x - side * 0.02, 0, 3.27), glass)
    # rim + net
    me = bpy.data.meshes.new(f"rim{side}")
    bm = bmesh.new()
    bmesh.ops.create_circle(bm, cap_ends=False, radius=0.23, segments=48)
    bm.to_mesh(me)
    bm.free()
    rim = link(bpy.data.objects.new(f"rim{side}", me))
    rim.location = (rim_x, 0, 3.05)
    rim.data.materials.append(rim_orange)
    sk = rim.modifiers.new("skin", "SKIN")
    for v in rim.data.skin_vertices[0].data:
        v.radius = (0.012, 0.012)
    rim.modifiers.new("sub", "SUBSURF").levels = 1
    rim.lightgroup = "base"
    bpy.ops.mesh.primitive_cone_add(vertices=24, radius1=0.23, radius2=0.15, depth=0.45,
                                    end_fill_type="NOTHING", location=(rim_x, 0, 2.825))
    net = bpy.context.active_object
    net.data.materials.append(net_mat)
    net.lightgroup = "base"
    # padded pole with an arm, behind the baseline
    pole_x = side * (CW / 2 + 1.6)
    cylinder(f"pole{side}", 0.16, 3.4, (pole_x, 0, 1.7), orange_pad, group="base")
    box(f"arm{side}", (abs(pole_x - board_x) + 0.1, 0.22, 0.3), ((pole_x + board_x) / 2, 0, 3.55),
        orange_pad, bevel=0.06)
    box(f"base{side}", (1.6, 1.4, 0.9), (pole_x + side * 0.5, 0, 0.45), orange_pad, bevel=0.15)
    box(f"base_pad{side}", (1.64, 1.44, 0.18), (pole_x + side * 0.5, 0, 0.75), black_gloss, bevel=0.05)

# ------------------------------------------------------------------------------------------------
# Light towers at the corners, string lights over the court
# ------------------------------------------------------------------------------------------------
tower_pts = []
for sx in (-1, 1):
    for sy in (-1, 1):
        x, y = sx * (CW / 2 + 3.5), sy * (CH / 2 + 3.5)
        tower_pts.append((x, y))
        cylinder(f"tower{sx}{sy}", 0.22, 14.0, (x, y, 7.0), black_gloss)
        head = box(f"head{sx}{sy}", (2.6, 0.5, 1.4), (x, y, 14.2), black_gloss, bevel=0.08)
        face_towards_eye(head)
        for i in range(3):
            for j in range(2):
                lamp = box(f"lamp{sx}{sy}{i}{j}", (0.7, 0.1, 0.55), (0, 0, 0), neon["white"])
                lamp.parent = head
                lamp.location = (-0.85 + 0.85 * i, -0.28, -0.32 + 0.64 * j)
        spot = bpy.data.lights.new(f"flood{sx}{sy}", "SPOT")
        spot.energy = 5000.0
        spot.spot_size = math.radians(70)
        spot.spot_blend = 0.6
        spot.color = (1.0, 0.92, 0.82)
        spot.shadow_soft_size = 1.2
        so = link(bpy.data.objects.new(f"flood{sx}{sy}", spot))
        so.location = (x * 0.92, y * 0.92, 13.6)
        so.rotation_euler = Vector((-x, -y, -13.0)).to_track_quat("-Z", "Y").to_euler()
        so.lightgroup = "base"

string_pts = []
for k, yline in enumerate((-4.0, 0.0, 4.0)):
    a = Vector((-(CW / 2 + 3.5), yline * 1.2, 11.5))
    b = Vector((CW / 2 + 3.5, yline * 1.2, 11.5))
    n = 46
    for i in range(n + 1):
        t = i / n
        p = a.lerp(b, t)
        p.z -= 2.2 * 4 * t * (1 - t)  # sag
        string_pts.append(p)
colors = ["gold", "pink", "cyan", "gold", "purple"]
for ci, cname in enumerate(colors):
    pts = [p for i, p in enumerate(string_pts) if i % len(colors) == ci]
    bulbs(f"string_{cname}", pts, 0.07, emissive(f"stringbulb_{cname}", NEON[cname], 14.0), "base")

# ------------------------------------------------------------------------------------------------
# Bleachers on both sidelines, LED boards around the court
# ------------------------------------------------------------------------------------------------
seat_mats = [seat_teal, seat_magenta, seat_gold]
for sy in (-1, 1):
    for row in range(6):
        y = sy * (CH / 2 + 5.0 + row * 0.9)
        z = 0.45 + row * 0.45
        box(f"step{sy}{row}", (22.0, 0.9, z * 2), (0, y, 0), concrete if row % 2 else mat(
            f"step_c{sy}{row}", (0.22, 0.2, 0.26), rough=0.7))
        for s in range(30):
            x = -10.5 + s * 0.72
            box(f"seat{sy}{row}{s}", (0.5, 0.42, 0.12), (x, y, z * 2 + 0.06),
                seat_mats[(s // 5 + row) % 3], bevel=0.04)
            box(f"back{sy}{row}{s}", (0.5, 0.08, 0.45), (x, y + sy * 0.2, z * 2 + 0.3),
                seat_mats[(s // 5 + row) % 3], bevel=0.03)

board_words_a = ["HOOP CITY", "SLAM", "BUCKETS", "ALLEY-OOP", "24/7 HOOPS", "BALL IS LIFE"]
board_words_b = ["GAME NIGHT", "DUNK", "AND ONE", "NOTHING BUT NET", "STREET KINGS", "BUZZER BEATER"]
board_y = CH / 2 + 3.2
k = 0
for sy in (-1, 1):
    for i in range(3):
        x = -9.0 + i * 9.0
        y = sy * board_y
        box(f"ledboard{sy}{i}", (8.4, 0.25, 1.0), (x, y, 0.5), led_dark, bevel=0.04)
        # the words face the court: -Y for the boards in front, +Y for the ones behind
        rot = (math.radians(90), 0, 0 if sy > 0 else math.radians(180))
        yy_t = y - sy * 0.14
        ca = ["pink", "cyan", "gold"][(i + (sy > 0)) % 3]
        cb = ["gold", "pink", "cyan"][(i + (sy > 0)) % 3]
        text(f"wa{sy}{i}", board_words_a[k % 6], (x, yy_t, 0.5), 0.62, ghost[ca], rot=rot,
             extrude=0.005, group="boardsA")
        text(f"wb{sy}{i}", board_words_b[k % 6], (x, yy_t - sy * 0.005, 0.5), 0.62, ghost[cb],
             rot=rot, extrude=0.005, group="boardsB")
        k += 1

# ------------------------------------------------------------------------------------------------
# Palm trees around the plaza
# ------------------------------------------------------------------------------------------------
def palm(x, y, h, lean):
    # trunk: a curved tube through a few points
    cu = bpy.data.curves.new("trunk", "CURVE")
    cu.dimensions = "3D"
    cu.bevel_depth = 0.28
    cu.bevel_resolution = 4
    sp = cu.splines.new("BEZIER")
    sp.bezier_points.add(2)
    pts = [Vector((x, y, 0)), Vector((x + lean[0] * 0.4, y + lean[1] * 0.4, h * 0.5)),
           Vector((x + lean[0], y + lean[1], h))]
    for bp, pt in zip(sp.bezier_points, pts):
        bp.co = pt
        bp.handle_left_type = bp.handle_right_type = "AUTO"
    tob = link(bpy.data.objects.new("trunk", cu))
    tob.data.materials.append(palm_trunk)
    tob.lightgroup = "base"
    top = Vector((x + lean[0], y + lean[1], h))
    # trunk taper via radius
    for i, bp in enumerate(sp.bezier_points):
        bp.radius = 1.0 - 0.35 * i / 2
    # fronds: arched tapered strips
    nf = 9
    for f in range(nf):
        ang = 2 * math.pi * f / nf + random.uniform(-0.2, 0.2)
        length = random.uniform(3.2, 4.2)
        me = bpy.data.meshes.new("frond")
        bm = bmesh.new()
        seg = 10
        prev = None
        for s in range(seg + 1):
            t = s / seg
            r = length * t
            droop = -1.6 * t * t + 0.6 * t
            c = top + Vector((math.cos(ang) * r, math.sin(ang) * r, droop))
            w = 0.55 * math.sin(math.pi * min(1.0, t * 1.1 + 0.05))
            side_v = Vector((-math.sin(ang), math.cos(ang), 0)) * w
            v1 = bm.verts.new(c - side_v + Vector((0, 0, -0.12 * w)))
            v2 = bm.verts.new(c)
            v3 = bm.verts.new(c + side_v + Vector((0, 0, -0.12 * w)))
            if prev:
                bm.faces.new((prev[0], prev[1], v2, v1))
                bm.faces.new((prev[1], prev[2], v3, v2))
            prev = (v1, v2, v3)
        bm.to_mesh(me)
        bm.free()
        for poly in me.polygons:
            poly.use_smooth = True
        fo = link(bpy.data.objects.new("frond", me))
        fo.data.materials.append(palm_leaf)
        sol = fo.modifiers.new("solid", "SOLIDIFY")
        sol.thickness = 0.03
        fo.lightgroup = "base"
    # coconuts
    for c in range(3):
        a = 2 * math.pi * c / 3
        sphere("coco", 0.16, top + Vector((math.cos(a) * 0.25, math.sin(a) * 0.25, -0.25)),
               palm_trunk)


for i in range(18):
    ang = 2 * math.pi * i / 18 + 0.1
    r = 26.0 + (i % 3) * 5.0
    x, y = math.cos(ang) * r * 1.15, math.sin(ang) * r
    # keep the area right in front (behind the big screen) a bit clearer
    palm(x, y, random.uniform(8.0, 12.0), (random.uniform(-1.2, 1.2), random.uniform(-1.2, 1.2)))

# ------------------------------------------------------------------------------------------------
# The Strip: hotels with lit windows and neon crowns
# ------------------------------------------------------------------------------------------------
def window_mat(name, facade, lit, density_seed):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nodes = nt.nodes
    p = nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (*facade, 1)
    p.inputs["Roughness"].default_value = 0.35
    p.inputs["Coat Weight"].default_value = 0.3
    tc = nodes.new("ShaderNodeTexCoord")
    sepx = nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(tc.outputs["Object"], sepx.inputs[0])
    addxy = nodes.new("ShaderNodeMath")
    addxy.operation = "ADD"
    nt.links.new(sepx.outputs["X"], addxy.inputs[0])
    nt.links.new(sepx.outputs["Y"], addxy.inputs[1])
    combv = nodes.new("ShaderNodeCombineXYZ")
    nt.links.new(addxy.outputs[0], combv.inputs["X"])
    nt.links.new(sepx.outputs["Z"], combv.inputs["Y"])
    br = nodes.new("ShaderNodeTexBrick")
    br.offset = 0.0
    br.squash = 1.0
    br.inputs["Scale"].default_value = 1.0
    br.inputs["Brick Width"].default_value = 4.2
    br.inputs["Row Height"].default_value = 4.6
    br.inputs["Mortar Size"].default_value = 1.0
    br.inputs["Bias"].default_value = density_seed
    br.inputs["Color1"].default_value = (*lit, 1)
    br.inputs["Color2"].default_value = (0.0, 0.0, 0.0, 1)
    br.inputs["Mortar"].default_value = (0, 0, 0, 1)
    nt.links.new(combv.outputs[0], br.inputs["Vector"])
    nt.links.new(br.outputs["Color"], p.inputs["Emission Color"])
    p.inputs["Emission Strength"].default_value = 1.8
    return m


def hotel(x, y, w, d, h, facade, lit, crown, seed=0.0):
    ob = box(f"hotel{x:.0f}{y:.0f}", (w, d, h), (x, y, h / 2), window_mat(
        f"win{x:.0f}{y:.0f}", facade, lit, seed), bevel=0.4)
    face_towards_eye(ob)
    # neon crown: glowing edge on top
    c = box(f"crown{x:.0f}{y:.0f}", (w + 0.6, d + 0.6, 0.9), (x, y, h + 0.45), neon[crown])
    c.rotation_euler = ob.rotation_euler
    cap = box(f"cap{x:.0f}{y:.0f}", (w * 0.6, d * 0.6, 6.0), (x, y, h + 3.5), mat(
        f"capm{x:.0f}{y:.0f}", facade, rough=0.3, coat=0.5), bevel=0.3)
    cap.rotation_euler = ob.rotation_euler
    return ob


strip = [
    # (azimuth deg from forward, distance, width, depth, height, facade, lit, crown)
    (-12, 210, 46, 30, 120, (0.75, 0.6, 0.35), (1.0, 0.75, 0.4), "gold"),
    (14, 260, 40, 34, 150, (0.18, 0.35, 0.45), (0.6, 0.9, 1.0), "cyan"),
    (32, 200, 50, 26, 95, (0.85, 0.85, 0.82), (1.0, 0.85, 0.55), "pink"),
    (-38, 230, 36, 30, 135, (0.45, 0.2, 0.38), (1.0, 0.55, 0.85), "purple"),
    (60, 170, 44, 28, 80, (0.6, 0.5, 0.42), (1.0, 0.8, 0.5), "gold"),
    (-62, 190, 40, 30, 105, (0.2, 0.25, 0.4), (0.7, 0.85, 1.0), "cyan"),
    (95, 220, 50, 34, 125, (0.8, 0.7, 0.55), (1.0, 0.8, 0.45), "red"),
    (-95, 200, 46, 30, 90, (0.55, 0.15, 0.2), (1.0, 0.6, 0.5), "gold"),
    (130, 240, 38, 30, 140, (0.15, 0.3, 0.3), (0.6, 1.0, 0.9), "green"),
    (-130, 230, 44, 30, 115, (0.75, 0.75, 0.8), (1.0, 0.9, 0.7), "pink"),
    (160, 260, 52, 30, 100, (0.6, 0.45, 0.3), (1.0, 0.75, 0.4), "orange"),
    (-160, 250, 40, 30, 130, (0.3, 0.2, 0.45), (0.85, 0.7, 1.0), "cyan"),
    (180, 300, 60, 30, 160, (0.8, 0.65, 0.4), (1.0, 0.78, 0.45), "gold"),
    (75, 330, 40, 30, 175, (0.25, 0.25, 0.3), (0.8, 0.9, 1.0), "purple"),
    (-80, 320, 46, 30, 165, (0.7, 0.55, 0.4), (1.0, 0.8, 0.5), "red"),
]
for i, (az, dist, w, d, h, fac, lit, crown) in enumerate(strip):
    a = math.radians(az)
    hotel(math.sin(a) * dist, math.cos(a) * dist, w, d, h, fac, lit, crown, seed=0.1 * (i % 4))

# ------------------------------------------------------------------------------------------------
# Landmarks: the basketball sphere, the ball tower, the arena marquee, the welcome sign
# ------------------------------------------------------------------------------------------------
def ball_material(name, strength):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nodes = nt.nodes
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    em = nodes.new("ShaderNodeEmission")
    tc = nodes.new("ShaderNodeTexCoord")
    sepx = nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(tc.outputs["Object"], sepx.inputs[0])
    # seams: |x| small, |z| small, and two curved seams |y| ~ f(z)
    def absmath(inp):
        n = nodes.new("ShaderNodeMath")
        n.operation = "ABSOLUTE"
        nt.links.new(inp, n.inputs[0])
        return n.outputs[0]

    def less(inp, v):
        n = nodes.new("ShaderNodeMath")
        n.operation = "LESS_THAN"
        n.inputs[1].default_value = v
        nt.links.new(inp, n.inputs[0])
        return n.outputs[0]

    def maxn(a, b):
        n = nodes.new("ShaderNodeMath")
        n.operation = "MAXIMUM"
        nt.links.new(a, n.inputs[0])
        nt.links.new(b, n.inputs[1])
        return n.outputs[0]

    s1 = less(absmath(sepx.outputs["X"]), 0.035)
    s2 = less(absmath(sepx.outputs["Z"]), 0.035)
    # curved seam: | |y| - 0.55*(1 - z^2)... | small
    zz = nodes.new("ShaderNodeMath")
    zz.operation = "MULTIPLY"
    nt.links.new(sepx.outputs["Z"], zz.inputs[0])
    nt.links.new(sepx.outputs["Z"], zz.inputs[1])
    curve = nodes.new("ShaderNodeMath")
    curve.operation = "MULTIPLY_ADD"
    curve.inputs[1].default_value = -0.55
    curve.inputs[2].default_value = 0.72
    nt.links.new(zz.outputs[0], curve.inputs[0])
    ay = absmath(sepx.outputs["Y"])
    diff = nodes.new("ShaderNodeMath")
    diff.operation = "SUBTRACT"
    nt.links.new(ay, diff.inputs[0])
    nt.links.new(curve.outputs[0], diff.inputs[1])
    s3 = less(absmath(diff.outputs[0]), 0.035)
    seam = maxn(maxn(s1, s2), s3)
    mixc = nodes.new("ShaderNodeMix")
    mixc.data_type = "RGBA"
    nt.links.new(seam, mixc.inputs["Factor"])
    mixc.inputs[6].default_value = (1.0, 0.28, 0.02, 1)
    mixc.inputs[7].default_value = (0.03, 0.01, 0.01, 1)
    # pebble texture shimmer
    vor = nodes.new("ShaderNodeTexNoise")
    vor.inputs["Scale"].default_value = 60.0
    mul = nodes.new("ShaderNodeMix")
    mul.data_type = "RGBA"
    mul.blend_type = "MULTIPLY"
    mul.inputs["Factor"].default_value = 0.25
    nt.links.new(mixc.outputs[2], mul.inputs[6])
    nt.links.new(vor.outputs["Color"], mul.inputs[7])
    nt.links.new(mul.outputs[2], em.inputs["Color"])
    em.inputs["Strength"].default_value = strength
    nt.links.new(em.outputs[0], out.inputs[0])
    return m


# The ball sphere arena (front right, like a famous one on the Strip)
a = math.radians(38)
# unit sphere scaled up: the seams are drawn in its object coordinates (-1..1)
sph = sphere("ballsphere", 1.0, (math.sin(a) * 150, math.cos(a) * 150, 44.0),
             ball_material("ballsphere", 0.7), subdiv=6)
sph.scale = (42.0, 42.0, 42.0)
sph.rotation_euler = (math.radians(20), math.radians(-25), math.radians(30))
# its base
cylinder("ballsphere_base", 30.0, 6.0, (sph.location.x, sph.location.y, 3.0), black_gloss)
# The ball tower (back left)
a = math.radians(-115)
tx, ty = math.sin(a) * 180, math.cos(a) * 180
cylinder("tower_shaft", 7.0, 160.0, (tx, ty, 80.0), mat("shaft", (0.85, 0.82, 0.78), rough=0.4),
         r2=4.5)
for i in range(4):
    ang = i * math.pi / 2
    box(f"tower_strip{i}", (0.6, 0.6, 150.0), (tx + math.cos(ang) * 6.0, ty + math.sin(ang) * 6.0,
                                                 78.0), neon["pink"])
cylinder("tower_pod", 16.0, 9.0, (tx, ty, 150.0), mat("pod", (0.9, 0.88, 0.85), rough=0.3), r2=12)
cylinder("tower_ring", 16.4, 1.2, (tx, ty, 149.0), neon["cyan"])
tb = sphere("tower_ball", 1.0, (tx, ty, 168.0), ball_material("towerball", 1.0), subdiv=5)
tb.scale = (11.0, 11.0, 11.0)


def marquee(name, center, w, h, facing, bulb_r, spacing, color_frame, words, word_color,
            word_size, sub=None, chase=True, flicker_word=False):
    """A sign board with chasing bulbs round its edge, facing the viewer."""
    cx, cy, cz = center
    root = bpy.data.objects.new(name, None)
    link(root)
    root.location = (cx, cy, 0)
    face_towards_eye(root)
    bd = box(name + "_board", (w, 0.6, h), (0, 0, cz), mat(name + "_bm", (0.05, 0.02, 0.08),
                                                         rough=0.3, coat=0.6), bevel=0.25)
    bd.parent = root
    fr = box(name + "_frame", (w + 0.5, 0.5, h + 0.5), (0, 0.12, cz), color_frame, bevel=0.2)
    fr.parent = root
    pts = rect_points(0.0, cz, w - 0.5, h - 0.5, spacing)
    for g in range(3):
        sel = [Vector((px, -0.36, pz)) for i, (px, pz) in enumerate(pts) if i % 3 == g]
        on = bulbs(name + f"_bulb{g}", sel, bulb_r, bulb_on if chase else bulb_off,
                   f"chase{g + 1}" if chase else "base")
        on.parent = root
    y = cz + (h * 0.12 if sub else 0)
    t = text(name + "_word", words, (0, -0.34, y), word_size, word_color,
             rot=(math.radians(90), 0, 0), extrude=0.06,
             group="flicker" if flicker_word else "base")
    t.parent = root
    if sub:
        s = text(name + "_sub", sub[0], (0, -0.34, cz - h * 0.28), word_size * 0.38, sub[1],
                 rot=(math.radians(90), 0, 0), extrude=0.04, font_file="C:/Windows/Fonts/ariblk.ttf")
        s.parent = root
    return root


# The welcome sign (behind left), the classic diamond board on two poles with a star on top
a = math.radians(-150)
wsx, wsy = math.sin(a) * 34, math.cos(a) * 34
ws = bpy.data.objects.new("welcome", None)
link(ws)
ws.location = (wsx, wsy, 0)
face_towards_eye(ws)
me = bpy.data.meshes.new("welcome_board")
bm = bmesh.new()
outline = [(-5.5, 0.0), (-4.6, 2.2), (0.0, 3.0), (4.6, 2.2), (5.5, 0.0), (4.6, -2.2), (0.0, -3.0),
           (-4.6, -2.2)]
vs = [bm.verts.new((x, 0.0, z)) for x, z in outline]
f = bm.faces.new(vs)
bmesh.ops.solidify(bm, geom=[f], thickness=0.4)
bm.to_mesh(me)
bm.free()
wb = link(bpy.data.objects.new("welcome_board", me))
wb.data.materials.append(white_paint)
wb.parent = ws
wb.location = (0, 0, 8.0)
wb.lightgroup = "base"
for px in (-2.0, 2.0):
    p = cylinder("welcome_pole", 0.25, 5.2, (px, 0.2, 2.6), white_paint)
    p.parent = ws
pts = []
for i in range(len(outline)):
    a0, a1 = Vector((outline[i][0], 0, outline[i][1])), Vector((outline[(i + 1) % len(outline)][0], 0,
                                                                outline[(i + 1) % len(outline)][1]))
    n = max(2, int((a1 - a0).length / 0.55))
    for j in range(n):
        q = a0.lerp(a1, j / n) * 0.9
        pts.append(Vector((q.x, -0.3, q.z + 8.0)))
for g in range(3):
    sel = [p for i, p in enumerate(pts) if i % 3 == g]
    b = bulbs(f"welcome_bulb{g}", sel, 0.14, bulb_on, f"chase{g + 1}")
    b.parent = ws
for (body, z, size, col, font) in (("WELCOME TO", 9.8, 0.85, neon["cyan"], "C:/Windows/Fonts/ariblk.ttf"),
                                   ("HOOP CITY", 8.0, 1.9, neon["red"], "C:/Windows/Fonts/impact.ttf"),
                                   ("OPEN 24/7", 6.3, 0.75, neon["gold"], "C:/Windows/Fonts/ariblk.ttf")):
    if body == "OPEN 24/7":
        col = neon_tube["gold"]
    t = text("welcome_" + body, body, (0, -0.25, z), size, col, extrude=0.05, font_file=font,
             group="flicker" if body == "OPEN 24/7" else "base")
    t.parent = ws
star = bpy.data.meshes.new("star")
bm = bmesh.new()
sv = []
for i in range(10):
    r = 1.0 if i % 2 == 0 else 0.42
    a = math.pi / 2 + i * math.pi / 5
    sv.append(bm.verts.new((r * math.cos(a), 0, r * math.sin(a))))
bm.faces.new(sv)
bmesh.ops.solidify(bm, geom=list(bm.faces), thickness=0.2)
bm.to_mesh(star)
bm.free()
so = link(bpy.data.objects.new("star", star))
so.data.materials.append(neon["gold"])
so.parent = ws
so.location = (0.0, -0.1, 11.8)
so.lightgroup = "base"

# The arena with its marquee (front left, around the big screen)
a = math.radians(-50)
marquee("arena", (math.sin(a) * 60, math.cos(a) * 60, 12.0), 30.0, 9.0, None, 0.22, 1.0,
        mat("arena_frame", (0.85, 0.65, 0.2), rough=0.2, metal=1.0), "HOOP CITY ARENA",
        neon["pink"], 4.2, sub=("TONIGHT: STREET KINGS vs THE LEGENDS", neon["cyan"]))
# arena building behind its sign
a2 = math.radians(-50)
arena = box("arena_bldg", (60, 30, 26), (math.sin(a2) * 78, math.cos(a2) * 78, 13.0),
            window_mat("arena_win", (0.3, 0.12, 0.35), (1.0, 0.5, 0.9), 0.2), bevel=0.6)
face_towards_eye(arena)
# Right side: a casino marquee with a flickering word
a = math.radians(70)
marquee("casino", (math.sin(a) * 48, math.cos(a) * 48, 9.0), 20.0, 7.0, None, 0.2, 0.9,
        mat("casino_frame", (0.9, 0.1, 0.25), rough=0.3, coat=0.5), "SLAM DUNK",
        neon_tube["gold"], 3.6, sub=("CASINO - HOOPS - 24 HRS", neon["white"]), flicker_word=True)
a2 = math.radians(70)
cas = box("casino_bldg", (40, 24, 20), (math.sin(a2) * 62, math.cos(a2) * 62, 10.0),
          window_mat("casino_win", (0.45, 0.08, 0.12), (1.0, 0.7, 0.4), 0.3), bevel=0.5)
face_towards_eye(cas)
# Behind right: a neon basketball sign on a pole
a = math.radians(125)
nbx, nby = math.sin(a) * 40, math.cos(a) * 40
nb = bpy.data.objects.new("neonball", None)
link(nb)
nb.location = (nbx, nby, 0)
face_towards_eye(nb)
cylinder("nb_pole", 0.35, 14.0, (0, 0.6, 7.0), black_gloss).parent = nb
for r, col in ((3.0, "orange"),):
    me = bpy.data.meshes.new("nbring")
    bm = bmesh.new()
    bmesh.ops.create_circle(bm, cap_ends=False, radius=r, segments=64)
    bm.to_mesh(me)
    bm.free()
    ring = link(bpy.data.objects.new("nbring", me))
    ring.data.materials.append(neon[col])
    ring.rotation_euler = (math.radians(90), 0, 0)
    ring.location = (0, 0, 17.0)
    sk = ring.modifiers.new("skin", "SKIN")
    for v in ring.data.skin_vertices[0].data:
        v.radius = (0.09, 0.09)
    ring.parent = nb
    ring.lightgroup = "flicker"
for ang in (0.0, math.pi / 2):
    seam_ob = box("nb_seam", (0.16, 0.16, 6.0), (0, -0.05, 17.0), neon["orange"], group="flicker")
    seam_ob.rotation_euler = (0, ang, 0)
    seam_ob.parent = nb
text("nb_word", "DUNK CITY", (0, -0.1, 12.2), 1.6, neon["pink"], group="base").parent = nb

# A giant LED billboard tower (behind right): its words swap (boardsA / boardsB) in the app.
a = math.radians(108)
bbx, bby = math.sin(a) * 75, math.cos(a) * 75
bbt = bpy.data.objects.new("billboard", None)
link(bbt)
bbt.location = (bbx, bby, 0)
face_towards_eye(bbt)
box("bb_tower", (16.0, 8.0, 52.0), (0, 4.5, 26.0), window_mat("bb_win", (0.12, 0.1, 0.2),
                                                              (0.8, 0.6, 1.0), 0.4), bevel=0.4).parent = bbt
screen_mat = bpy.data.materials.new("bb_screen")
screen_mat.use_nodes = True
snt = screen_mat.node_tree
snt.nodes.clear()
s_out = snt.nodes.new("ShaderNodeOutputMaterial")
s_em = snt.nodes.new("ShaderNodeEmission")
s_tc = snt.nodes.new("ShaderNodeTexCoord")
s_sep = snt.nodes.new("ShaderNodeSeparateXYZ")
snt.links.new(s_tc.outputs["Generated"], s_sep.inputs[0])
s_ramp = snt.nodes.new("ShaderNodeValToRGB")
s_ramp.color_ramp.elements[0].color = (0.25, 0.02, 0.35, 1)
s_ramp.color_ramp.elements[1].color = (0.9, 0.18, 0.05, 1)
snt.links.new(s_sep.outputs["Z"], s_ramp.inputs["Fac"])
snt.links.new(s_ramp.outputs["Color"], s_em.inputs["Color"])
s_em.inputs["Strength"].default_value = 1.6
snt.links.new(s_em.outputs[0], s_out.inputs[0])
box("bb_screen", (22.0, 0.6, 30.0), (0, 0.0, 34.0), screen_mat).parent = bbt
box("bb_frame", (23.2, 0.5, 31.2), (0, 0.25, 34.0), black_gloss, bevel=0.3).parent = bbt
bb_ball = sphere("bb_ball", 1.0, (0, -0.6, 39.0), ball_material("bb_ballmat", 1.6), subdiv=4)
bb_ball.scale = (6.5, 0.3, 6.5)
bb_ball.parent = bbt
for body, z, size, col, grp in (("SLAM DUNK", 28.0, 4.2, "white", "boardsA"),
                                ("CONTEST", 23.5, 3.4, "gold", "boardsA"),
                                ("STREET", 28.0, 4.6, "cyan", "boardsB"),
                                ("KINGS", 23.5, 4.2, "pink", "boardsB")):
    text("bb_" + body, body, (0, -0.75, z), size, ghost_emissive("bbw_" + body, NEON[col], 9.0),
         group=grp).parent = bbt

# Searchlights behind the arena and the casino: soft beams into the sky.
beam_mat = bpy.data.materials.new("beam")
beam_mat.use_nodes = True
bnt = beam_mat.node_tree
bnt.nodes.clear()
b_out = bnt.nodes.new("ShaderNodeOutputMaterial")
b_em = bnt.nodes.new("ShaderNodeEmission")
b_em.inputs["Color"].default_value = (0.75, 0.85, 1.0, 1)
b_tr = bnt.nodes.new("ShaderNodeBsdfTransparent")
b_add = bnt.nodes.new("ShaderNodeAddShader")
b_lw = bnt.nodes.new("ShaderNodeLayerWeight")
b_lw.inputs["Blend"].default_value = 0.5
b_tc = bnt.nodes.new("ShaderNodeTexCoord")
b_sep = bnt.nodes.new("ShaderNodeSeparateXYZ")
bnt.links.new(b_tc.outputs["Generated"], b_sep.inputs[0])
# strength = facing (soft edges) * fade along the beam (generated z: 0 at the lamp, 1 at the tip)
b_face = bnt.nodes.new("ShaderNodeMath")
b_face.operation = "SUBTRACT"
b_face.inputs[0].default_value = 1.0
bnt.links.new(b_lw.outputs["Facing"], b_face.inputs[1])
b_fade = bnt.nodes.new("ShaderNodeMath")
b_fade.operation = "SUBTRACT"
b_fade.inputs[0].default_value = 1.0
bnt.links.new(b_sep.outputs["Z"], b_fade.inputs[1])
b_mul = bnt.nodes.new("ShaderNodeMath")
b_mul.operation = "MULTIPLY"
bnt.links.new(b_face.outputs[0], b_mul.inputs[0])
bnt.links.new(b_fade.outputs[0], b_mul.inputs[1])
b_k = bnt.nodes.new("ShaderNodeMath")
b_k.operation = "MULTIPLY"
b_k.inputs[1].default_value = 0.42 if NIGHT else 0.22
bnt.links.new(b_mul.outputs[0], b_k.inputs[0])
bnt.links.new(b_k.outputs[0], b_em.inputs["Strength"])
bnt.links.new(b_em.outputs[0], b_add.inputs[0])
bnt.links.new(b_tr.outputs[0], b_add.inputs[1])
bnt.links.new(b_add.outputs[0], b_out.inputs[0])
for i, (az, dist, tilt, turn) in enumerate(((-58, 95, 18, -30), (-46, 95, 12, 25), (-53, 100, 25, 5),
                                            (74, 80, 15, 35), (64, 80, 20, -20), (150, 120, 14, 10))):
    a = math.radians(az)
    base_pt = Vector((math.sin(a) * dist, math.cos(a) * dist, 2.0))
    cone = cylinder(f"beam{i}", 0.7, 320.0, (0, 0, 0), beam_mat, verts=24, r2=6.0)
    cone.data.transform(Matrix.Translation((0, 0, 160.0)))  # base at the lamp
    cone.location = base_pt
    cone.rotation_euler = (math.radians(tilt), 0, math.radians(turn))
    cone.visible_shadow = False
    cylinder(f"searchlamp{i}", 1.2, 1.4, base_pt + Vector((0, 0, -0.6)), neon["white"])

# Distant mountains (a dark ring) under the sky glow
me = bpy.data.meshes.new("mountains")
bm = bmesh.new()
ring_lo, ring_hi = [], []
n = 180
for i in range(n):
    a = 2 * math.pi * i / n
    hgt = 60 + 90 * abs(math.sin(a * 3.0 + 1.0)) * (0.6 + 0.4 * math.sin(a * 7.0)) + 40 * random.random()
    r = 2200.0
    ring_lo.append(bm.verts.new((math.cos(a) * r, math.sin(a) * r, -5)))
    ring_hi.append(bm.verts.new((math.cos(a) * r, math.sin(a) * r, hgt)))
for i in range(n):
    j = (i + 1) % n
    bm.faces.new((ring_lo[i], ring_lo[j], ring_hi[j], ring_hi[i]))
bm.to_mesh(me)
bm.free()
mt = link(bpy.data.objects.new("mountains", me))
mt.data.materials.append(mat("mountain", (0.05, 0.02, 0.08) if NIGHT else (0.22, 0.10, 0.20),
                             rough=0.9))
mt.lightgroup = "base"

# ------------------------------------------------------------------------------------------------
# Cameras and compositor
# ------------------------------------------------------------------------------------------------
cam_data = bpy.data.cameras.new("cam")
cam = link(bpy.data.objects.new("cam", cam_data))
cam.location = EYE
scene.camera = cam
cam_data.clip_start = 0.05
cam_data.clip_end = 5000.0

scene.use_nodes = True
ct = scene.node_tree
ct.nodes.clear()
rl = ct.nodes.new("CompositorNodeRLayers")
fo = ct.nodes.new("CompositorNodeOutputFile")
fo.base_path = OUT
fo.format.file_format = "OPEN_EXR"
fo.format.color_depth = "16"
fo.format.exr_codec = "ZIP"
fo.file_slots.clear()
comp = ct.nodes.new("CompositorNodeComposite")
ct.links.new(rl.outputs["Image"], comp.inputs["Image"])


def setup_outputs(prefix):
    fo.file_slots.clear()
    for node in [n for n in ct.nodes if n.bl_idname == "CompositorNodeDenoise"]:
        ct.nodes.remove(node)
    for g in GROUPS:
        sock_name = f"Combined_{g}"
        if sock_name not in rl.outputs:
            print("MISSING PASS", sock_name)
            continue
        dn = ct.nodes.new("CompositorNodeDenoise")
        dn.prefilter = "ACCURATE"
        ct.links.new(rl.outputs[sock_name], dn.inputs["Image"])
        ct.links.new(rl.outputs["Denoising Normal"], dn.inputs["Normal"])
        ct.links.new(rl.outputs["Denoising Albedo"], dn.inputs["Albedo"])
        fo.file_slots.new(f"{prefix}_{g}_")
        ct.links.new(dn.outputs[0], fo.inputs[-1])
    fo.file_slots.new(f"{prefix}_mist_")
    ct.links.new(rl.outputs["Mist"], fo.inputs[-1])


# Cube faces, GL order +X -X +Y -Y +Z -Z, in OpenXR axes (Y up, -Z forward), as tools\Vr360Cube.cs:
# texel (s, t) of face f looks along F + s*R + t*D. The GL faces are mirror images of a camera's
# picture, so each face is rendered with right = -R and flipped back by vegas_post.py.
FACES = [
    ((1, 0, 0), (0, 0, -1), (0, -1, 0)),
    ((-1, 0, 0), (0, 0, 1), (0, -1, 0)),
    ((0, 1, 0), (1, 0, 0), (0, 0, 1)),
    ((0, -1, 0), (1, 0, 0), (0, 0, -1)),
    ((0, 0, 1), (1, 0, 0), (0, -1, 0)),
    ((0, 0, -1), (-1, 0, 0), (0, -1, 0)),
]


def xr_to_blender(v):
    return Vector((v[0], -v[2], v[1]))


if MODE == "inspect":
    bpy.context.view_layer.update()
    for ob in scene.objects:
        if ob.type == "LIGHT" and ob.data.type == "SPOT":
            d = ob.matrix_world.to_3x3() @ Vector((0, 0, -1))
            print("SPOT", ob.name, tuple(round(c, 2) for c in ob.location), tuple(round(c, 2) for c in d))
        if ob.type in ("MESH", "FONT", "CURVE"):
            bb = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
            lo = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
            hi = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
            # anything that covers the centre of the court between the floor and the eyes
            if lo.x < 0.5 and hi.x > -0.5 and lo.y < 0.5 and hi.y > -0.5 and hi.z > -0.05 and lo.z < 1.6:
                print("OVER_COURT", ob.name, tuple(round(c, 2) for c in lo), tuple(round(c, 2) for c in hi))
    sys.exit(0)
if MODE == "debug":
    # a plain look down at the court, the whole render as one picture
    scene.render.resolution_x = 960
    scene.render.resolution_y = 540
    cam.rotation_euler = (math.radians(55), 0, 0)
    cam_data.angle = math.radians(100)
    if os.environ.get("VEGAS_BIRD"):
        cam.location = (0, -32, 22)
        cam.rotation_euler = (math.radians(55), 0, 0)
        cam_data.angle = math.radians(70)
    print("COURT_MATS", [m.name for m in court.data.materials], court.location[:], court.scale[:],
          court.hide_render, court.visible_camera)
    scene.view_settings.view_transform = "AgX"
    scene.render.filepath = os.path.join(OUT, f"{VARIANT}_debug.png")
    fo.file_slots.clear()
    bpy.ops.render.render(write_still=True)
elif MODE == "preview":
    scene.render.resolution_x = 2048
    scene.render.resolution_y = 1024
    cam_data.type = "PANO"
    cam_data.panorama_type = "EQUIRECTANGULAR"
    cam.rotation_euler = (math.radians(90), 0, 0)
    setup_outputs(f"{VARIANT}_pano")
    scene.frame_set(1)
    bpy.ops.render.render(write_still=False)
else:
    n = int(round(FACE * OVERSCAN))
    scene.render.resolution_x = n
    scene.render.resolution_y = n
    cam_data.type = "PERSP"
    cam_data.sensor_fit = "HORIZONTAL"
    cam_data.angle = 2 * math.atan(OVERSCAN)
    only = [int(f) for f in os.environ.get("VEGAS_FACES", "0,1,2,3,4,5").split(",")]
    for fi in only:
        F, R, D = FACES[fi]
        f = xr_to_blender(F)
        up = -xr_to_blender(D)
        right = -xr_to_blender(R)  # the camera's own right; flipped back later
        # camera axes: X = right, Y = up, Z = -forward
        m = Matrix((right, up, -f)).transposed()
        cam.rotation_euler = m.to_euler()
        setup_outputs(f"{VARIANT}_face{fi}")
        scene.frame_set(1)
        bpy.ops.render.render(write_still=False)
        print("FACE_DONE", fi, flush=True)
print("SCENE_DONE", VARIANT, MODE, flush=True)
