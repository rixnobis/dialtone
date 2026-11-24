# Builds the desk scene in Blender, saves it as assets/desk.blend, and writes one OBJ per
# object plus src/desk_layout.hh with where the game places the moving parts.
#
#   blender -b --factory-startup -P tools/build_desk.py
#
# Then tools/make_assets.sh turns the OBJs into meshbins. Everything is flat vertex colour,
# no textures. Modelled Blender style (Z up, Y away from the camera), 1 unit = 1.0_ws; the
# export turns that into the engine's Y-down space. The desk is built big (the phone is
# about a unit wide) so small parts like the keys keep enough fixed point precision.

import math
import os

import bmesh
import bpy

def script_path():
    # run with -P, __file__ is the script; run from the Text Editor, it is only the text's name,
    # so take the path the text was opened from
    p = os.path.abspath(__file__)
    if os.path.isfile(p):
        return p
    for t in bpy.data.texts:
        path = bpy.path.abspath(t.filepath) if t.filepath else ""
        if os.path.basename(path) == "build_desk.py" and os.path.isfile(path):
            return path
    raise SystemExit("open tools/build_desk.py from the repo in the Text Editor (Text > Open), then Run Script")


HERE = os.path.dirname(script_path())
ROOT = os.path.dirname(HERE)
OBJ_DIR = os.path.join(ROOT, "build", "obj")
BLEND = os.path.join(ROOT, "blender", "desk.blend")
LAYOUT = os.path.join(ROOT, "src", "desk_layout.hh")

# phone keypad, on the sloped top of the phone body
PHONE_X = -1.15
SLOPE_FRONT_Z = 0.22
SLOPE_BACK_Z = 0.62
SLOPE_DEPTH = 0.8
SLOPE = math.atan2(SLOPE_BACK_Z - SLOPE_FRONT_Z, SLOPE_DEPTH)
KEY_PITCH_X = 0.3
KEY_PITCH_S = 0.18
KEY_S0 = 0.68

WOOD = (0.55, 0.35, 0.2)
WOOD_DARK = (0.38, 0.22, 0.12)
PHONE_RED = (0.72, 0.13, 0.11)
PHONE_RED_DARK = (0.5, 0.08, 0.07)
KEY = (0.85, 0.8, 0.68)

layout = {}


def reset():
    if bpy.app.background and not os.environ.get("DESK_SOFT_RESET"):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        return
    # inside the Blender UI a factory reset would also reset the user's preferences, so only
    # clear what this script builds: every object and mesh in the open file
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    for m in list(bpy.data.meshes):
        bpy.data.meshes.remove(m)


def new_object(name, bm):
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def paint(bm, layer, faces, colour):
    for f in faces:
        for loop in f.loops:
            loop[layer] = (*colour, 1.0)


def add_grid(bm, layer, lo, hi, z, step, colour, holes=()):
    # a flat, upward-facing rectangle at height z cut into tiles of about `step` on a side;
    # a tile whose centre falls inside one of `holes` ((x0, y0, x1, y1)) is left out
    nx = max(1, round((hi[0] - lo[0]) / step))
    ny = max(1, round((hi[1] - lo[1]) / step))
    xs = [lo[0] + (hi[0] - lo[0]) * i / nx for i in range(nx + 1)]
    ys = [lo[1] + (hi[1] - lo[1]) * j / ny for j in range(ny + 1)]
    v = [[bm.verts.new((x, y, z)) for x in xs] for y in ys]
    for j in range(ny):
        for i in range(nx):
            cx, cy = (xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2
            if any(h[0] < cx < h[2] and h[1] < cy < h[3] for h in holes):
                continue
            paint(bm, layer, [bm.faces.new([v[j][i], v[j][i + 1], v[j + 1][i + 1], v[j + 1][i]])], colour)


def add_front(bm, layer, x0, x1, y, z0, z1, step, colour, holes=()):
    # a wall facing the camera (-y), cut at every hole edge and then into tiles no bigger than
    # `step`, so the small parts in front of it sort in front; tiles inside `holes` ((x0, z0, x1, z1))
    # are left out
    def cuts(a, b, edges):
        points = sorted(set([a, b] + [e for e in edges if a < e < b]))
        out = [points[0]]
        for p, q in zip(points, points[1:]):
            n = max(1, math.ceil((q - p) / step - 1e-9))
            out += [p + (q - p) * i / n for i in range(1, n + 1)]
        return out
    xs = cuts(x0, x1, [h[0] for h in holes] + [h[2] for h in holes])
    zs = cuts(z0, z1, [h[1] for h in holes] + [h[3] for h in holes])
    v = [[bm.verts.new((x, y, z)) for x in xs] for z in zs]
    for j in range(len(zs) - 1):
        for i in range(len(xs) - 1):
            cx, cz = (xs[i] + xs[i + 1]) / 2, (zs[j] + zs[j + 1]) / 2
            if any(h[0] < cx < h[2] and h[1] < cz < h[3] for h in holes):
                continue
            paint(bm, layer, [bm.faces.new([v[j][i], v[j][i + 1], v[j + 1][i + 1], v[j + 1][i]])], colour)


def add_box(bm, layer, lo, hi, top, side, bottom=None, skip=()):
    # an axis aligned box; top/side/bottom colours so it reads as solid without lighting
    verts = [bm.verts.new((x, y, z)) for z in (lo[2], hi[2]) for y in (lo[1], hi[1]) for x in (lo[0], hi[0])]
    # index = x + 2y + 4z
    quads = {
        "bottom": (0, 2, 3, 1),
        "top": (4, 5, 7, 6),
        "front": (0, 1, 5, 4),
        "back": (2, 6, 7, 3),
        "left": (0, 4, 6, 2),
        "right": (1, 3, 7, 5),
    }
    shade = {"front": 1.0, "back": 0.7, "left": 0.8, "right": 0.8}
    for name, idx in quads.items():
        if name in skip:
            continue
        f = bm.faces.new([verts[i] for i in idx])
        if name == "top":
            c = top
        elif name == "bottom":
            c = bottom or tuple(v * 0.4 for v in side)
        else:
            c = tuple(v * shade[name] for v in side)
        paint(bm, layer, [f], c)


def add_prism(bm, layer, x0, x1, profile, top, side, skip=None):
    # extrude a y/z profile (counter-clockwise seen from +x) along x; edge `skip` is left open
    left = [bm.verts.new((x0, y, z)) for y, z in profile]
    right = [bm.verts.new((x1, y, z)) for y, z in profile]
    n = len(profile)
    paint(bm, layer, [bm.faces.new(list(reversed(left)))], tuple(v * 0.8 for v in side))
    paint(bm, layer, [bm.faces.new(right)], tuple(v * 0.8 for v in side))
    for i in range(n):
        j = (i + 1) % n
        if i == skip:
            continue
        f = bm.faces.new([left[i], left[j], right[j], right[i]])
        f.normal_update()
        c = top if f.normal.z > 0.3 else tuple(v * (1.0 if f.normal.y < -0.5 else 0.6) for v in side)
        paint(bm, layer, [f], c)


def start(name):
    bm = bmesh.new()
    layer = bm.loops.layers.float_color.new("Col")
    return bm, layer


def build():
    reset()

    # desk and the wall behind it, baked in place
    bm, col = start("DESK")
    # the renderer does not cull back faces and the ordering table sorts each face by its average
    # depth, so a big face sorts in front of whatever stands on it: the desk top is tiles, with no
    # tile under the notepad, and the faces the seated camera never sees are not built at all
    NOTEPAD = (-0.2, -0.3, 0.6, 0.5)
    add_box(bm, col, (-3.4, -0.7, -0.15), (3.4, 2.5, 0.0), WOOD, WOOD_DARK, skip=("top", "bottom", "back", "front"))
    add_front(bm, col, -3.4, 3.4, -0.7, -0.15, 0.0, 0.2, WOOD_DARK)
    # tiles small enough that none needs the renderer's subdivision, whose split edges crack against
    # an unsplit neighbour
    add_grid(bm, col, (-3.4, -0.7), (3.4, 2.5), 0.0, 0.2, WOOD, holes=[NOTEPAD])
    add_box(bm, col, (-3.4, -0.75, -1.4), (3.4, -0.65, -0.15), WOOD_DARK, WOOD_DARK)
    add_box(bm, col, (-5.0, 2.5, -1.5), (5.0, 2.6, 0.9), (0.2, 0.25, 0.22), (0.22, 0.3, 0.27))
    add_box(bm, col, (-5.0, 2.5, 0.9), (5.0, 2.6, 4.0), (0.2, 0.3, 0.32), (0.26, 0.36, 0.4))
    # a calendar on the wall and a notepad on the desk, so the desk is not a bare plank
    add_box(bm, col, (-0.5, 2.45, 1.4), (0.5, 2.5, 2.5), (0.9, 0.9, 0.85), (0.92, 0.9, 0.82))
    add_box(bm, col, (-0.5, 2.44, 2.2), (0.5, 2.46, 2.5), (0.7, 0.15, 0.1), (0.75, 0.18, 0.12))
    add_box(bm, col, (NOTEPAD[0], NOTEPAD[1], 0.0), (NOTEPAD[2], NOTEPAD[3], 0.03), (0.95, 0.92, 0.6), (0.8, 0.78, 0.5), skip=("bottom", "top"))
    add_grid(bm, col, NOTEPAD[:2], NOTEPAD[2:], 0.03, 0.2, (0.95, 0.92, 0.6))
    new_object("DESK", bm)

    # the phone body: a wedge with the keypad on its slope and a cradle at the back
    bm, col = start("PHONE")
    profile = [(0.0, 0.0), (1.25, 0.0), (1.25, SLOPE_BACK_Z), (SLOPE_DEPTH, SLOPE_BACK_Z), (0.0, SLOPE_FRONT_Z)]
    add_prism(bm, col, PHONE_X - 0.65, PHONE_X + 0.65, profile, PHONE_RED, PHONE_RED, skip=3)
    for x in (PHONE_X - 0.5, PHONE_X + 0.38):
        add_box(bm, col, (x, 0.9, SLOPE_BACK_Z), (x + 0.12, 1.15, SLOPE_BACK_Z + 0.1), PHONE_RED_DARK, PHONE_RED_DARK)
    # the slope, one strip per key row: the ordering table sorts a face by its average depth, so
    # one big slope face would sort in front of the far keys and paint over them
    rows = [KEY_S0 - r * KEY_PITCH_S for r in range(4)]
    cuts = sorted(set([0.0, SLOPE_DEPTH] + [(rows[i] + rows[i + 1]) / 2 for i in range(3)] + [rows[0] + KEY_PITCH_S / 2, rows[3] - KEY_PITCH_S / 2]))
    for a, b in zip(cuts, cuts[1:]):
        za, zb = (SLOPE_FRONT_Z + c * math.tan(SLOPE) for c in (a, b))
        # keypad well in the middle, red either side
        for x0, x1, c in ((PHONE_X - 0.65, PHONE_X - 0.48, PHONE_RED), (PHONE_X - 0.48, PHONE_X + 0.48, (0.2, 0.05, 0.05)), (PHONE_X + 0.48, PHONE_X + 0.65, PHONE_RED)):
            v = [bm.verts.new(p) for p in ((x0, a, za), (x1, a, za), (x1, b, zb), (x0, b, zb))]
            paint(bm, col, [bm.faces.new(v)], c)
    new_object("PHONE", bm)

    # handset, modelled at the origin; the game lays it on the cradle and lifts it off-hook
    bm, col = start("HANDSET")
    add_box(bm, col, (-0.6, -0.07, 0.0), (0.6, 0.07, 0.08), PHONE_RED, PHONE_RED)
    add_box(bm, col, (-0.66, -0.12, -0.06), (-0.4, 0.12, 0.06), PHONE_RED_DARK, PHONE_RED)
    add_box(bm, col, (0.4, -0.12, -0.06), (0.66, 0.12, 0.06), PHONE_RED_DARK, PHONE_RED)
    new_object("HANDSET", bm)
    layout["HANDSET"] = (PHONE_X, 1.02, SLOPE_BACK_Z + 0.16)

    # key caps, already tilted to the slope, three tints the game swaps between
    for name, colour in (("KEY", KEY), ("KEYSEL", (1.0, 0.7, 0.15)), ("KEYLIT", (1.0, 1.0, 0.75))):
        bm, col = start(name)
        add_box(bm, col, (-0.125, -0.075, 0.0), (0.125, 0.075, 0.05), colour, tuple(c * 0.75 for c in colour))
        bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=__import__("mathutils").Matrix.Rotation(SLOPE, 3, "X"))
        new_object(name, bm)

    keys = []
    for row in range(4):
        for c in range(3):
            s = KEY_S0 - row * KEY_PITCH_S
            keys.append((PHONE_X + (c - 1) * KEY_PITCH_X, s, SLOPE_FRONT_Z + s * math.tan(SLOPE) + 0.01))
    layout["KEYS"] = keys

    # redial button, beside the keypad
    bm, col = start("REDIAL")
    add_box(bm, col, (-0.07, -0.07, 0.0), (0.07, 0.07, 0.06), (1.0, 0.25, 0.1), (0.7, 0.15, 0.05))
    bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=__import__("mathutils").Matrix.Rotation(SLOPE, 3, "X"))
    new_object("REDIAL", bm)
    s = 0.2
    layout["REDIAL"] = (PHONE_X + 0.56, s, SLOPE_FRONT_Z + s * math.tan(SLOPE) + 0.01)

    # second line: a smaller black phone, handset baked on
    bm, col = start("PHONE2")
    x2 = 0.05
    profile = [(0.0, 0.0), (0.9, 0.0), (0.9, 0.32), (0.55, 0.32), (0.0, 0.18)]
    add_prism(bm, col, x2 - 0.45, x2 + 0.45, [(y + 0.45, z) for y, z in profile], (0.16, 0.16, 0.18), (0.12, 0.12, 0.14))
    add_box(bm, col, (x2 - 0.5, 1.05, 0.32), (x2 + 0.5, 1.2, 0.42), (0.2, 0.2, 0.22), (0.12, 0.12, 0.14))
    new_object("PHONE2", bm)
    layout["LAMP_LINE2"] = (x2 + 0.3, 0.7, 0.26)

    # the autodialer: a beige box with a lamp, wired to nothing in particular
    bm, col = start("AUTODIAL")
    add_box(bm, col, (-0.35, 1.6, 0.0), (0.35, 2.2, 0.35), (0.8, 0.76, 0.62), (0.7, 0.66, 0.52))
    add_box(bm, col, (-0.25, 1.58, 0.08), (0.25, 1.6, 0.2), (0.1, 0.1, 0.1), (0.1, 0.12, 0.1))
    new_object("AUTODIAL", bm)
    layout["LAMP_AUTO"] = (0.2, 1.8, 0.35)

    # the radio
    bm, col = start("RADIO")
    # the front is tiles with the speaker and the dial window cut out, so neither sorts behind it
    add_box(bm, col, (0.7, 0.5, 0.0), (2.5, 1.4, 1.0), (0.45, 0.26, 0.14), (0.42, 0.24, 0.12), skip=("front",))
    add_front(bm, col, 0.7, 2.5, 0.5, 0.0, 1.0, 0.3, (0.42, 0.24, 0.12), holes=[(0.85, 0.15, 1.55, 0.85), (1.65, 0.55, 2.35, 0.8)])
    add_box(bm, col, (0.85, 0.48, 0.15), (1.55, 0.5, 0.85), (0.12, 0.09, 0.07), (0.12, 0.09, 0.07))
    for z in (0.3, 0.45, 0.6, 0.75):
        add_box(bm, col, (0.85, 0.47, z), (1.55, 0.48, z + 0.03), (0.3, 0.2, 0.12), (0.3, 0.2, 0.12))
    add_box(bm, col, (1.65, 0.48, 0.55), (2.35, 0.5, 0.8), (0.95, 0.85, 0.55), (0.95, 0.85, 0.55))
    for x in (1.8, 2.2):
        add_box(bm, col, (x - 0.08, 0.4, 0.2), (x + 0.08, 0.5, 0.36), (0.15, 0.12, 0.1), (0.2, 0.16, 0.12))
    new_object("RADIO", bm)
    layout["NEEDLE"] = [(1.8, 0.465, 0.56), (2.2, 0.465, 0.56)]
    layout["LAMP_AIR"] = (1.6, 0.9, 1.0)

    bm, col = start("NEEDLE")
    add_box(bm, col, (-0.015, -0.01, 0.0), (0.015, 0.0, 0.22), (0.9, 0.1, 0.05), (0.9, 0.1, 0.05))
    new_object("NEEDLE", bm)

    for name, colour in (("LAMP", (0.3, 0.07, 0.05)), ("LAMPLIT", (1.0, 0.25, 0.12))):
        bm, col = start(name)
        add_box(bm, col, (-0.08, -0.08, 0.0), (0.08, 0.08, 0.1), colour, tuple(c * 0.8 for c in colour))
        new_object(name, bm)

    # phone books mode: a line board with a lamp per line where the radio stood, and the books
    bm, col = start("LINEBOARD")
    add_box(bm, col, (0.7, 0.55, 0.0), (1.65, 1.35, 0.3), (0.16, 0.16, 0.18), (0.2, 0.2, 0.23), skip=("top", "bottom", "front"))
    add_front(bm, col, 0.7, 1.65, 0.55, 0.0, 0.3, 0.2, (0.2, 0.2, 0.23))
    add_grid(bm, col, (0.7, 0.55), (1.65, 1.35), 0.3, 0.19, (0.16, 0.16, 0.18))
    new_object("LINEBOARD", bm)
    layout["LINE_LAMPS"] = [(0.8 + (i % 5) * 0.19, 0.75 + (i // 5) * 0.3, 0.3) for i in range(10)]

    # phone books mode: the autodialer is its own box, like an alarm clock, with a dark window the
    # game draws an LED readout over; its front is tiles around the window so the window sorts in front
    bm, col = start("AUTOBOX")
    BX0, BX1, BY, BZ = -0.45, 0.5, 1.0, 0.5
    WINDOW = (BX0 + 0.1, 0.12, BX1 - 0.1, 0.42)
    BEIGE, BEIGE_SIDE = (0.8, 0.76, 0.62), (0.7, 0.66, 0.52)
    add_box(bm, col, (BX0, BY, 0.0), (BX1, BY + 0.4, BZ), BEIGE, BEIGE_SIDE, skip=("front", "top", "bottom", "back"))
    add_front(bm, col, BX0, BX1, BY, 0.0, BZ, 0.2, BEIGE_SIDE, holes=[WINDOW])
    add_grid(bm, col, (BX0, BY), (BX1, BY + 0.4), BZ, 0.2, BEIGE)
    add_box(bm, col, (WINDOW[0], BY - 0.02, WINDOW[1]), (WINDOW[2], BY, WINDOW[3]), (0.08, 0.03, 0.03), (0.09, 0.03, 0.03), skip=("top", "bottom", "back"))
    new_object("AUTOBOX", bm)
    layout["AUTOBOX_LED"] = ((WINDOW[0] + WINDOW[2]) / 2, BY - 0.02, (WINDOW[1] + WINDOW[3]) / 2)
    layout["AUTOBOX_LAMP"] = (BX1 - 0.15, BY + 0.2, BZ)

    # one phone book, yellow pages; the game stacks as many as are left to dial, and a grey one per
    # finished book on the done pile behind
    for name, cover in (("BOOK", (0.95, 0.78, 0.12)), ("BOOKDONE", (0.5, 0.48, 0.4))):
        bm, col = start(name)
        # pages on the sides, the cover on top in tiles small enough to need no subdivision
        add_box(bm, col, (-0.4, -0.3, 0.0), (0.4, 0.3, 0.12), cover, (0.95, 0.93, 0.85), skip=("bottom", "top"))
        add_grid(bm, col, (-0.4, -0.3), (0.4, 0.3), 0.12, 0.2, cover)
        new_object(name, bm)
    layout["BOOK_STACK"] = (2.15, 0.85, 0.0)
    layout["BOOK_DONE"] = (2.15, 1.75, 0.0)
    layout["BOOK_HEIGHT"] = 0.13

    # a mug, because every desk has one
    bm, col = start("MUG")
    ret = bmesh.ops.create_cone(bm, cap_ends=True, segments=8, radius1=0.22, radius2=0.22, depth=0.5)
    bmesh.ops.translate(bm, verts=ret["verts"], vec=(2.95, 0.2, 0.25))
    for f in bm.faces:
        f.normal_update()
        paint(bm, col, [f], (0.25, 0.1, 0.06) if f.normal.z > 0.5 else (0.85, 0.85, 0.9))
    new_object("MUG", bm)

    os.makedirs(os.path.dirname(BLEND), exist_ok=True)
    # just the scene and what it uses: a full save also stores the UI state, file browser paths included
    bpy.data.libraries.write(BLEND, {bpy.context.scene}, compress=True)


def to_engine(v):
    # blender (x, y away, z up) -> engine (x, y down, z away)
    return (v[0], -v[2], v[1])


def write_obj(obj, path):
    mesh = obj.data
    colours = mesh.color_attributes["Col"].data
    m = obj.matrix_world
    with open(path, "w") as f:
        f.write("o %s\n" % obj.name)
        # nothing here is textured, but MeshManager::LoadMesh indexes the uv table for every
        # face corner, so give every corner the same dummy uv rather than none
        f.write("vt 0 0\n")
        n = 1
        for poly in mesh.polygons:
            idx = []
            for li in poly.loop_indices:
                p = to_engine(m @ mesh.vertices[mesh.loops[li].vertex_index].co)
                c = colours[li].color
                rgb = [min(255, int(round(ch * 255))) for ch in c[:3]]
                f.write("v %.5f %.5f %.5f %d %d %d\n" % (*p, *rgb))
                idx.append(n)
                n += 1
            nrm = to_engine(m.to_3x3() @ poly.normal)
            f.write("vn %.5f %.5f %.5f\n" % nrm)
            f.write("f " + " ".join("%d/1/%d" % (i, poly.index + 1) for i in idx) + "\n")


def fp(v):
    return "%.4f_ws" % v


def vec(p):
    e = to_engine(p)
    return "{%s, %s, %s}" % tuple(fp(c) for c in e)


def write_layout():
    lines = [
        "// generated by tools/build_desk.py, do not edit",
        "#pragma once",
        "",
        '#include "core/world_defs.hh"',
        "",
        "#include <psyqo/vector.hh>",
        "",
        "namespace desk {",
        "",
        "using namespace psyqo::fixed_point_literals;",
        "",
    ]
    for key in ("HANDSET", "REDIAL", "LAMP_LINE2", "LAMP_AUTO", "LAMP_AIR"):
        lines.append("constexpr psyqo::Vec3 %s = %s;" % (key, vec(layout[key])))
    lines.append("constexpr psyqo::Vec3 KEYS[12] = {")
    lines += ["\t%s," % vec(k) for k in layout["KEYS"]]
    lines.append("};")
    lines.append("constexpr psyqo::Vec3 NEEDLE[2] = {%s};" % ", ".join(vec(n) for n in layout["NEEDLE"]))
    lines.append("constexpr psyqo::Vec3 LINE_LAMPS[10] = {")
    lines += ["\t%s," % vec(k) for k in layout["LINE_LAMPS"]]
    lines.append("};")
    for key in ("BOOK_STACK", "BOOK_DONE", "AUTOBOX_LED", "AUTOBOX_LAMP"):
        lines.append("constexpr psyqo::Vec3 %s = %s;" % (key, vec(layout[key])))
    lines.append("constexpr psyqo::FixedPoint<> BOOK_HEIGHT = %s;" % fp(layout["BOOK_HEIGHT"]))
    lines += ["", "} // namespace desk", ""]
    with open(LAYOUT, "w") as f:
        f.write("\n".join(lines))


build()
os.makedirs(OBJ_DIR, exist_ok=True)
for obj in bpy.context.scene.objects:
    write_obj(obj, os.path.join(OBJ_DIR, obj.name + ".obj"))
write_layout()
print("DESK: wrote %d objects" % len(bpy.context.scene.objects))
