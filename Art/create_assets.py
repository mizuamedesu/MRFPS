"""MRFPS asset generator for Blender 5.2 (background, bpy only).

Builds the six MRFPS static meshes procedurally, saves an editable .blend,
exports one FBX (+ GLB) per asset, writes manifest.json and renders a
1280x720 preview board.

Run from the project root (see Art/README.md):

    blender --background --factory-startup --threads 2 --python-exit-code 1 \
        --python Art/create_assets.py [-- --no-render] [-- --engine cycles]

Conventions (Blender space == asset local space):
    metres, +Z up, +X forward/fire/normal, +Y = left of +X (right-handed).
    UE import flips Y (Blender +Y -> UE -Y), so shapes keep their handedness.

The script only writes inside Art/Generated and Art/Preview. It never deletes
directories; it overwrites/removes only the exact files it produces.
"""

import hashlib
import json
import math
import os
import re
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

# --------------------------------------------------------------------------
# Paths
# --------------------------------------------------------------------------

ART_DIR = os.path.dirname(os.path.abspath(__file__))
GEN_DIR = os.path.join(ART_DIR, "Generated")
EXPORT_DIR = os.path.join(GEN_DIR, "Exports")
PREVIEW_DIR = os.path.join(ART_DIR, "Preview")
BLEND_PATH = os.path.join(GEN_DIR, "MRFPS_Assets.blend")
MANIFEST_PATH = os.path.join(GEN_DIR, "manifest.json")
BOARD_PATH = os.path.join(PREVIEW_DIR, "AssetBoard.png")

GENERATOR_VERSION = 2  # v2: board renders only preview copies; head fits its hit sphere

# --------------------------------------------------------------------------
# Palette. Slot names are the contract with UE; colours are preview values.
# "ue" is the suggested unlit emissive colour (the six existing ones match
# Source/MRFPSEditor/Private/MRFPSSetupCommandlet.cpp).
# --------------------------------------------------------------------------

PALETTE = {
    "M_Graphite": dict(base=(0.060, 0.066, 0.078), metal=0.15, rough=0.62, emit=0.0,
                       ue=(0.035, 0.040, 0.050)),
    "M_Trim":     dict(base=(0.600, 0.640, 0.690), metal=0.35, rough=0.34, emit=0.0,
                       ue=(0.550, 0.600, 0.660)),
    "M_White":    dict(base=(0.780, 0.810, 0.840), metal=0.0, rough=0.45, emit=0.0,
                       ue=(0.850, 0.880, 0.920)),
    "M_Blue":     dict(base=(0.020, 0.260, 0.520), metal=0.0, rough=0.40, emit=1.15,
                       emit_color=(0.040, 0.550, 1.000), ue=(0.040, 0.550, 1.000)),
    "M_Orange":   dict(base=(0.520, 0.110, 0.015), metal=0.0, rough=0.40, emit=1.15,
                       emit_color=(1.000, 0.240, 0.035), ue=(1.000, 0.240, 0.035)),
    "M_Wall":     dict(base=(0.030, 0.150, 0.250), metal=0.0, rough=0.50, emit=0.95,
                       emit_color=(0.080, 0.400, 0.650), ue=(0.080, 0.400, 0.650)),
    "M_Shield":   dict(base=(0.040, 0.380, 0.240), metal=0.0, rough=0.50, emit=0.85,
                       emit_color=(0.100, 0.950, 0.600), ue=(0.100, 0.950, 0.600)),
    "M_Tracer":   dict(base=(0.500, 0.400, 0.050), metal=0.0, rough=0.40, emit=1.40,
                       emit_color=(1.000, 0.800, 0.100), ue=(1.000, 0.800, 0.100)),
}

SHARP_ANGLE = math.radians(33.0)


def log(msg):
    print("[MRFPS-ART] " + msg, flush=True)


def parse_args():
    import argparse
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(prog="create_assets.py")
    parser.add_argument("--no-render", action="store_true", help="skip AssetBoard.png")
    parser.add_argument("--engine", choices=("eevee", "cycles"), default="eevee",
                        help="board renderer; cycles = CPU fallback")
    parser.add_argument("--samples", type=int, default=0, help="override render samples")
    return parser.parse_args(argv)


def rnd(values, digits=5):
    return [round(float(v), digits) + 0.0 for v in values]


# --------------------------------------------------------------------------
# Scene reset and materials
# --------------------------------------------------------------------------

def reset_scene():
    scene = bpy.context.scene
    for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.lights,
                 bpy.data.cameras, bpy.data.curves, bpy.data.worlds, bpy.data.images):
        ids = list(coll)
        if ids:
            bpy.data.batch_remove(ids)
    for child in list(scene.collection.children):
        scene.collection.children.unlink(child)
    for coll in list(bpy.data.collections):
        bpy.data.collections.remove(coll)
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0
    try:
        scene.unit_settings.length_unit = "METERS"
    except (TypeError, AttributeError):
        pass
    return scene


def _node_tree(idblock):
    if getattr(idblock, "node_tree", None) is None:
        try:
            idblock.use_nodes = True  # deprecated no-op in 5.x, required in 4.x
        except (AttributeError, TypeError):
            pass
    return idblock.node_tree


def _set_input(node, name, value):
    sock = node.inputs.get(name)
    if sock is not None:
        try:
            sock.default_value = value
        except (TypeError, ValueError):
            pass


def make_materials():
    mats = {}
    for name, p in PALETTE.items():
        mat = bpy.data.materials.new(name)
        tree = _node_tree(mat)
        tree.nodes.clear()
        out = tree.nodes.new("ShaderNodeOutputMaterial")
        out.location = (300, 0)
        bsdf = tree.nodes.new("ShaderNodeBsdfPrincipled")
        bsdf.location = (0, 0)
        _set_input(bsdf, "Base Color", (*p["base"], 1.0))
        _set_input(bsdf, "Metallic", p["metal"])
        _set_input(bsdf, "Roughness", p["rough"])
        if p["emit"] > 0.0:
            _set_input(bsdf, "Emission Color", (*p["emit_color"], 1.0))
            _set_input(bsdf, "Emission Strength", p["emit"])
        tree.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
        shown = p.get("emit_color", p["base"])
        mat.diffuse_color = (*shown, 1.0)
        mat.metallic = p["metal"]
        mat.roughness = p["rough"]
        mat["ue_unlit_color"] = list(p["ue"])
        mats[name] = mat
    return mats


def emission_material(name, color, strength=1.0):
    mat = bpy.data.materials.new(name)
    tree = _node_tree(mat)
    tree.nodes.clear()
    out = tree.nodes.new("ShaderNodeOutputMaterial")
    emit = tree.nodes.new("ShaderNodeEmission")
    emit.inputs["Color"].default_value = (*color, 1.0)
    emit.inputs["Strength"].default_value = strength
    tree.links.new(emit.outputs["Emission"], out.inputs["Surface"])
    mat.diffuse_color = (*color, 1.0)
    return mat


# --------------------------------------------------------------------------
# Geometry toolkit (bmesh). Every part is a closed solid with outward
# normals; feature edges get a 1-segment chamfer whose faces carry their own
# material (usually M_Trim) so edges stay readable under unlit shading.
# --------------------------------------------------------------------------

class MeshBuilder:
    def __init__(self, name, slots):
        self.name = name
        self.slots = list(slots)
        self.bm = bmesh.new()

    def m(self, slot):
        return self.slots.index(slot)


def _map(axes, a, b, w):
    if axes == "XZ":   # profile in XZ, extruded along Y
        return (a, w, b)
    if axes == "YZ":   # profile in YZ, extruded along X
        return (w, a, b)
    if axes == "XY":   # profile in XY, extruded along Z
        return (a, b, w)
    raise ValueError(axes)


def _verts(mb, points, mtx=None):
    new = mb.bm.verts.new
    if mtx is None:
        return [new(Vector(p)) for p in points]
    return [new(mtx @ Vector(p)) for p in points]


def _ccw(profile):
    area = 0.0
    for i, (a0, b0) in enumerate(profile):
        a1, b1 = profile[(i + 1) % len(profile)]
        area += a0 * b1 - a1 * b0
    return list(profile) if area >= 0.0 else list(reversed(profile))


def bevel(mb, edges, width, mat):
    edges = list(dict.fromkeys(e for e in edges if e is not None and e.is_valid))
    if not edges or width <= 0.0:
        return []
    res = bmesh.ops.bevel(mb.bm, geom=edges, offset=width, offset_type="OFFSET",
                          segments=1, profile=0.5, affect="EDGES", clamp_overlap=True)
    faces = res.get("faces", [])
    for f in faces:
        f.material_index = mat
    return list(faces)


def _finish(mb, faces, mat, bevel_edges, width, bevel_mat):
    for f in faces:
        f.material_index = mat
    bmesh.ops.recalc_face_normals(mb.bm, faces=faces)
    return faces + bevel(mb, bevel_edges, width, mat if bevel_mat is None else bevel_mat)


def prism(mb, profile, w0, w1, axes, mat, mtx=None, bevel_w=0.0, bevel_mat=None, bevel_on="caps"):
    """Extrude a 2D polygon (may be concave) between w0 and w1."""
    prof = _ccw(profile)
    lo = _verts(mb, [_map(axes, a, b, w0) for a, b in prof], mtx)
    hi = _verts(mb, [_map(axes, a, b, w1) for a, b in prof], mtx)
    bm = mb.bm
    faces = [bm.faces.new(lo[::-1]), bm.faces.new(hi)]
    n = len(prof)
    for i in range(n):
        j = (i + 1) % n
        faces.append(bm.faces.new((lo[i], lo[j], hi[j], hi[i])))
    if bevel_on == "caps":
        edges = list(faces[0].edges) + list(faces[1].edges)
    elif bevel_on == "hi":
        edges = list(faces[1].edges)
    elif bevel_on == "lo":
        edges = list(faces[0].edges)
    elif bevel_on == "all":
        edges = [e for f in faces for e in f.edges]
    else:
        edges = []
    return _finish(mb, faces, mat, edges, bevel_w, bevel_mat)


def box(mb, size, mat, center=(0.0, 0.0, 0.0), mtx=None, bevel_w=0.0, bevel_mat=None, bevel_on="all"):
    hx, hy, hz = size[0] * 0.5, size[1] * 0.5, size[2] * 0.5
    cx, cy, cz = center
    prof = [(cy - hy, cz - hz), (cy + hy, cz - hz), (cy + hy, cz + hz), (cy - hy, cz + hz)]
    return prism(mb, prof, cx - hx, cx + hx, "YZ", mat, mtx, bevel_w, bevel_mat, bevel_on)


def ngon(r, n, cx=0.0, cy=0.0, phase=0.0):
    return [(cx + r * math.cos(phase + 2.0 * math.pi * k / n),
             cy + r * math.sin(phase + 2.0 * math.pi * k / n)) for k in range(n)]


def superellipse(ha, hb, n, count, phase=0.0):
    pts = []
    for k in range(count):
        t = phase + 2.0 * math.pi * k / count
        c, s = math.cos(t), math.sin(t)
        pts.append((ha * math.copysign(abs(c) ** (2.0 / n), c),
                    hb * math.copysign(abs(s) ** (2.0 / n), s)))
    return pts


def rounded_rect(hw, hh, r, seg, cx=0.0, cy=0.0, splits=()):
    """CCW rounded (seg>1) or chamfered (seg==1) rectangle.

    Returns (points, kinds) where kinds[i] describes segment points[i]->points[i+1]:
    'arc' for corner segments, 'side<k>' for straight pieces cut at `splits`.
    """
    corners = [(hw - r, hh - r, 0.0), (-hw + r, hh - r, 90.0),
               (-hw + r, -hh + r, 180.0), (hw - r, -hh + r, 270.0)]
    pts, kinds = [], []
    for c, (ccx, ccy, a0) in enumerate(corners):
        arc = []
        for k in range(seg + 1):
            a = math.radians(a0 + 90.0 * k / seg)
            arc.append((cx + ccx + r * math.cos(a), cy + ccy + r * math.sin(a)))
        pts.extend(arc)
        kinds.extend(["arc"] * seg)
        nxt = corners[(c + 1) % 4]
        start = (cx + nxt[0] + r * math.cos(math.radians(nxt[2])),
                 cy + nxt[1] + r * math.sin(math.radians(nxt[2])))
        end = arc[-1]
        for t in splits:
            pts.append((end[0] + (start[0] - end[0]) * t, end[1] + (start[1] - end[1]) * t))
        kinds.extend("side%d" % k for k in range(len(splits) + 1))
    return pts, kinds


def ring_prism(mb, outer, inner, w0, w1, axes, mats, mtx=None,
               outer_bevel=0.0, outer_mat=None, inner_bevel=0.0, inner_mat=None):
    """Closed frame between two corresponding outlines (same point count)."""
    assert len(outer) == len(inner)
    outer, inner = _ccw(outer), _ccw(inner)
    n = len(outer)
    ol = _verts(mb, [_map(axes, a, b, w0) for a, b in outer], mtx)
    oh = _verts(mb, [_map(axes, a, b, w1) for a, b in outer], mtx)
    il = _verts(mb, [_map(axes, a, b, w0) for a, b in inner], mtx)
    ih = _verts(mb, [_map(axes, a, b, w1) for a, b in inner], mtx)
    bm = mb.bm
    faces = []
    for i in range(n):
        j = (i + 1) % n
        m = mats[i] if isinstance(mats, (list, tuple)) else mats
        for quad in ((oh[i], oh[j], ih[j], ih[i]), (ol[j], ol[i], il[i], il[j]),
                     (ol[i], ol[j], oh[j], oh[i]), (il[j], il[i], ih[i], ih[j])):
            f = bm.faces.new(quad)
            f.material_index = m
            faces.append(f)
    bmesh.ops.recalc_face_normals(bm, faces=faces)
    oe = [bm.edges.get((lp[i], lp[(i + 1) % n])) for lp in (ol, oh) for i in range(n)]
    ie = [bm.edges.get((lp[i], lp[(i + 1) % n])) for lp in (il, ih) for i in range(n)]
    faces += bevel(mb, oe, outer_bevel, outer_mat)
    faces += bevel(mb, ie, inner_bevel, inner_mat)
    return faces


def loft(mb, rings, mat, caps=("ngon", "ngon"), poles=(None, None), mtx=None):
    """Skin closed rings (equal point counts) and cap both ends."""
    rows = [_verts(mb, ring, mtx) for ring in rings]
    n = len(rings[0])
    bm = mb.bm
    faces = []
    for r in range(len(rows) - 1):
        a, b = rows[r], rows[r + 1]
        for i in range(n):
            j = (i + 1) % n
            faces.append(bm.faces.new((a[i], a[j], b[j], b[i])))
    for row, cap, pole in ((rows[0], caps[0], poles[0]), (rows[-1], caps[1], poles[1])):
        if cap == "ngon":
            faces.append(bm.faces.new(row))
        elif cap == "fan":
            p = _verts(mb, [pole], mtx)[0]
            for i in range(n):
                faces.append(bm.faces.new((row[i], row[(i + 1) % n], p)))
    return _finish(mb, faces, mat, [], 0.0, None)


def closed_tube(mb, loops, mat, mtx=None):
    """Torus-like band: loops are connected cyclically (last back to first)."""
    rows = [_verts(mb, lp, mtx) for lp in loops]
    n = len(loops[0])
    bm = mb.bm
    faces = []
    for r in range(len(rows)):
        a, b = rows[r], rows[(r + 1) % len(rows)]
        for i in range(n):
            j = (i + 1) % n
            faces.append(bm.faces.new((a[i], a[j], b[j], b[i])))
    return _finish(mb, faces, mat, [], 0.0, None)


def shell_patch(mb, fn, us, vs, off_out, off_in, mat, mtx=None, bevel_w=0.0, bevel_mat=None,
                face_mat=None):
    """Thick curved plate: fn(u, v, offset) -> point on an offset surface.

    face_mat(a, b) may return a material index for outer quad (a, b), giving
    flush inlays without extra protrusion.
    """
    O = [_verts(mb, [fn(u, v, off_out) for v in vs], mtx) for u in us]
    I = [_verts(mb, [fn(u, v, off_in) for v in vs], mtx) for u in us]
    nu, nv = len(us), len(vs)
    bm = mb.bm
    faces, inlays = [], []
    for a in range(nu - 1):
        for b in range(nv - 1):
            outer = bm.faces.new((O[a][b], O[a + 1][b], O[a + 1][b + 1], O[a][b + 1]))
            faces.append(outer)
            faces.append(bm.faces.new((I[a][b + 1], I[a + 1][b + 1], I[a + 1][b], I[a][b])))
            m = face_mat(a, b) if face_mat is not None else None
            if m is not None:
                inlays.append((outer, m))
    for a in range(nu - 1):
        faces.append(bm.faces.new((O[a][0], I[a][0], I[a + 1][0], O[a + 1][0])))
        faces.append(bm.faces.new((O[a + 1][-1], I[a + 1][-1], I[a][-1], O[a][-1])))
    for b in range(nv - 1):
        faces.append(bm.faces.new((O[0][b + 1], I[0][b + 1], I[0][b], O[0][b])))
        faces.append(bm.faces.new((O[-1][b], I[-1][b], I[-1][b + 1], O[-1][b + 1])))
    edges = [bm.edges.get((O[a][0], O[a + 1][0])) for a in range(nu - 1)]
    edges += [bm.edges.get((O[a][-1], O[a + 1][-1])) for a in range(nu - 1)]
    edges += [bm.edges.get((O[0][b], O[0][b + 1])) for b in range(nv - 1)]
    edges += [bm.edges.get((O[-1][b], O[-1][b + 1])) for b in range(nv - 1)]
    for f in faces:
        f.material_index = mat
    for f, m in inlays:  # set before bevel so rebuilt faces inherit it
        f.material_index = m
    bmesh.ops.recalc_face_normals(bm, faces=faces)
    return faces + bevel(mb, edges, bevel_w, mat if bevel_mat is None else bevel_mat)


def linspace(a, b, n):
    return [a + (b - a) * k / (n - 1) for k in range(n)]


def rot(axis, degrees):
    return Matrix.Rotation(math.radians(degrees), 4, axis)


MIRROR_Y = Matrix.Scale(-1.0, 4, (0.0, 1.0, 0.0))
OCT_PHASE = math.pi / 8.0  # octagon with flats on the principal axes


def finalize_mesh(mb, mats):
    """Clean, shade (sharp by angle or material change), box-UV and convert."""
    bm = mb.bm
    bmesh.ops.dissolve_degenerate(bm, dist=1e-7, edges=list(bm.edges))
    loose_edges = [e for e in bm.edges if not e.link_faces]
    if loose_edges:
        bmesh.ops.delete(bm, geom=loose_edges, context="EDGES")
    loose_verts = [v for v in bm.verts if not v.link_faces]
    if loose_verts:
        bmesh.ops.delete(bm, geom=loose_verts, context="VERTS")
    bm.normal_update()
    for f in bm.faces:
        f.smooth = True
    for e in bm.edges:
        lf = e.link_faces
        e.smooth = (len(lf) == 2 and lf[0].material_index == lf[1].material_index
                    and lf[0].normal.angle(lf[1].normal, math.pi) <= SHARP_ANGLE)
    uv = bm.loops.layers.uv.new("UVMap")
    for f in bm.faces:
        n = f.normal
        axis = max(range(3), key=lambda i: abs(n[i]))
        for loop in f.loops:
            co = loop.vert.co
            pair = ((co.y, co.z), (co.x, co.z), (co.x, co.y))[axis]
            loop[uv].uv = (pair[0] * 2.0, pair[1] * 2.0)
    mesh = bpy.data.meshes.new(mb.name)
    bm.to_mesh(mesh)
    bm.free()
    for slot in mb.slots:
        mesh.materials.append(mats[slot])
    mesh.update()
    return mesh


# --------------------------------------------------------------------------
# SM_MRPistol: origin = grip attach on the bore axis, +X fire, muzzle +X 0.22.
# Matches AMRFPSPawn::Muzzle() (Right + forward * 22 cm).
# --------------------------------------------------------------------------

def build_pistol():
    mb = MeshBuilder("SM_MRPistol", ["M_Blue", "M_Graphite", "M_Trim", "M_White", "M_Tracer"])
    B, G, T, W, TR = (mb.m(s) for s in mb.slots)
    # Upper receiver: side profile in XZ extruded across Y, outlined in trim.
    prism(mb, [(-0.0985, -0.004), (0.165, -0.004), (0.165, 0.010), (0.142, 0.028),
               (-0.084, 0.028), (-0.0985, 0.014)], -0.0275, 0.0275, "XZ", G,
          bevel_w=0.003, bevel_mat=T)
    # Sight deck: white plate with a team-colour aiming line over the bore.
    prism(mb, [(-0.078, 0.026), (0.128, 0.026), (0.120, 0.0315), (-0.072, 0.0315)],
          -0.0145, 0.0145, "XZ", W, bevel_w=0.0015, bevel_mat=T)
    prism(mb, [(-0.068, 0.030), (0.112, 0.030), (0.106, 0.0355), (-0.064, 0.0355)],
          -0.0042, 0.0042, "XZ", B)
    # Side energy rails; they set the 0.065 m overall width.
    rail = [(-0.060, 0.004), (0.098, 0.004), (0.118, 0.017), (-0.048, 0.017)]
    for s in (1.0, -1.0):
        prism(mb, rail, s * 0.0255, s * 0.0325, "XZ", B)
    # Rear power-cell window, visible to the holder.
    window, _ = rounded_rect(0.013, 0.0065, 0.003, 1, cx=0.0, cy=0.0075)
    prism(mb, window, -0.100, -0.094, "YZ", B)
    # Lower frame.
    prism(mb, [(-0.072, -0.026), (0.122, -0.026), (0.150, -0.016), (0.150, -0.002),
               (-0.072, -0.002)], -0.022, 0.022, "XZ", G, bevel_w=0.0025, bevel_mat=T)
    # Emitter barrel on the bore axis (Z = 0): shroud, team ring, collar, lens.
    prism(mb, ngon(0.0195, 8, phase=OCT_PHASE), 0.150, 0.204, "YZ", G,
          bevel_w=0.002, bevel_mat=T, bevel_on="hi")
    prism(mb, ngon(0.0215, 8, phase=OCT_PHASE), 0.170, 0.182, "YZ", B)
    prism(mb, ngon(0.0235, 8, phase=OCT_PHASE), 0.200, 0.2145, "YZ", T,
          bevel_w=0.0015, bevel_mat=G)
    prism(mb, ngon(0.0135, 8, phase=OCT_PHASE), 0.212, 0.220, "YZ", TR)
    # Raked grip with raised side panels.
    prism(mb, [(0.016, -0.020), (-0.040, -0.020), (-0.062, -0.106), (-0.004, -0.106),
               (0.006, -0.066)], -0.0165, 0.0165, "XZ", G, bevel_w=0.003, bevel_mat=T)
    panel = [(0.004, -0.032), (-0.032, -0.032), (-0.050, -0.096), (-0.012, -0.096),
             (-0.004, -0.066)]
    for s in (1.0, -1.0):
        prism(mb, panel, s * 0.0145, s * 0.0190, "XZ", G, bevel_w=0.002, bevel_mat=T, bevel_on="hi")
    # Power cell at the grip base with a team-colour band.
    prism(mb, [(-0.001, -0.100), (-0.064, -0.100), (-0.066, -0.1245), (-0.004, -0.1245)],
          -0.0195, 0.0195, "XZ", W, bevel_w=0.0025, bevel_mat=T)
    band = [(-0.007, -0.1095), (-0.0615, -0.1095), (-0.062, -0.1150), (-0.0075, -0.1150)]
    for s in (1.0, -1.0):
        prism(mb, band, s * 0.0180, s * 0.0210, "XZ", B)
    # Trigger guard (open U closed by grip and frame) and trigger blade.
    prism(mb, [(0.070, -0.024), (0.070, -0.050), (0.056, -0.066), (0.000, -0.066),
               (0.0015, -0.056), (0.050, -0.056), (0.060, -0.046), (0.060, -0.024)],
          -0.0085, 0.0085, "XZ", G, bevel_w=0.0015, bevel_mat=T)
    prism(mb, [(0.030, -0.024), (0.040, -0.024), (0.034, -0.050), (0.027, -0.050)],
          -0.0045, 0.0045, "XZ", T)
    return mb


# --------------------------------------------------------------------------
# SM_MRHand: tracked LEFT hand, origin = wrist, +X fingers, +Z dorsal,
# palm faces -Z, thumb on -Y (UE: +Y after the importer's Y flip).
# --------------------------------------------------------------------------

def build_hand():
    mb = MeshBuilder("SM_MRHand", ["M_Blue", "M_Graphite", "M_Trim", "M_White"])
    B, G, T, W = (mb.m(s) for s in mb.slots)
    # Wrist cuff with a team band.
    prism(mb, superellipse(0.0355, 0.0255, 2.6, 16), -0.012, 0.016, "YZ", G,
          bevel_w=0.0025, bevel_mat=T)
    prism(mb, superellipse(0.0375, 0.0275, 2.6, 16), -0.002, 0.006, "YZ", B)
    # Palm block (top-view outline extruded through the hand thickness).
    prism(mb, [(0.004, -0.030), (0.030, -0.042), (0.092, -0.040), (0.100, -0.028),
               (0.100, 0.034), (0.090, 0.042), (0.012, 0.036), (0.004, 0.026)],
          -0.015, 0.016, "XY", G, bevel_w=0.003, bevel_mat=T)
    # Dorsal armour plate with a +X chevron so inspectors read hand direction.
    prism(mb, [(0.018, -0.029), (0.082, -0.032), (0.090, -0.022), (0.090, 0.030),
               (0.082, 0.035), (0.022, 0.029)], 0.012, 0.0205, "XY", W,
          bevel_w=0.002, bevel_mat=T, bevel_on="hi")
    prism(mb, [(0.038, -0.017), (0.054, -0.017), (0.072, 0.002), (0.054, 0.021),
               (0.038, 0.021), (0.056, 0.002)], 0.018, 0.0245, "XY", B)
    box(mb, (0.018, 0.074, 0.016), G, center=(0.093, 0.002, 0.014), bevel_w=0.0025, bevel_mat=T)
    # Palm shield emitter (-Z side) in a trim ring.
    prism(mb, ngon(0.025, 8, 0.054, 0.002, OCT_PHASE), -0.0175, -0.010, "XY", T)
    prism(mb, ngon(0.0175, 8, 0.054, 0.002, OCT_PHASE), -0.0195, -0.012, "XY", B)
    # Fingers: (y, splay deg, proximal length, distal length, width).
    for y, splay, l1, l2, w in ((-0.025, -4.0, 0.036, 0.030, 0.0165),
                                (-0.007, 0.0, 0.040, 0.032, 0.0170),
                                (0.011, 4.0, 0.037, 0.030, 0.0160),
                                (0.028, 9.0, 0.029, 0.024, 0.0140)):
        base = Matrix.Translation((0.096, y, 0.001)) @ rot("Z", splay) @ rot("Y", 6.0)
        box(mb, (l1 - 0.003, w, 0.0165), G, center=(l1 * 0.5, 0.0, 0.0), mtx=base,
            bevel_w=0.0025, bevel_mat=T)
        tip = base @ Matrix.Translation((l1, 0.0, 0.0)) @ rot("Y", 10.0)
        box(mb, (l2 - 0.002, w * 0.94, 0.015), W, center=(l2 * 0.5, 0.0, 0.0), mtx=tip,
            bevel_w=0.0025, bevel_mat=T)
    # Thumb along the -Y side of the palm.
    t0 = Matrix.Translation((0.018, -0.031, -0.006)) @ rot("Z", -16.0) @ rot("Y", 6.0)
    box(mb, (0.030, 0.019, 0.017), G, center=(0.013, 0.0, 0.0), mtx=t0, bevel_w=0.0025, bevel_mat=T)
    t1 = t0 @ Matrix.Translation((0.028, 0.0, 0.0)) @ rot("Y", 6.0)
    box(mb, (0.022, 0.0175, 0.016), G, center=(0.0125, 0.0, 0.0), mtx=t1, bevel_w=0.0025, bevel_mat=T)
    t2 = t1 @ Matrix.Translation((0.025, 0.0, 0.0)) @ rot("Y", 8.0)
    box(mb, (0.019, 0.0165, 0.015), W, center=(0.0105, 0.0, 0.0), mtx=t2, bevel_w=0.0025, bevel_mat=T)
    return mb


# --------------------------------------------------------------------------
# SM_MRMiniShield: centre origin, normal +X, fills its 0.04 x 0.44 x 0.44 box.
# --------------------------------------------------------------------------

def build_shield():
    mb = MeshBuilder("SM_MRMiniShield", ["M_Shield", "M_Graphite", "M_Trim", "M_White"])
    S, G, T, W = (mb.m(s) for s in mb.slots)
    outer, kinds = rounded_rect(0.220, 0.220, 0.050, 3, splits=(0.3, 0.7))
    inner, _ = rounded_rect(0.178, 0.178, 0.022, 3, splits=(0.3, 0.7))
    rim_mats = [G if k == "side1" else W for k in kinds]  # white corner brackets
    ring_prism(mb, outer, inner, -0.020, 0.020, "YZ", rim_mats,
               outer_bevel=0.004, outer_mat=T, inner_bevel=0.004, inner_mat=S)
    field, _ = rounded_rect(0.188, 0.188, 0.030, 3)
    prism(mb, field, -0.006, 0.006, "YZ", S)
    # Cross struts and emitter hub (same language as the wall).
    box(mb, (0.020, 0.372, 0.026), G, bevel_w=0.003, bevel_mat=T)
    box(mb, (0.020, 0.026, 0.372), G, bevel_w=0.003, bevel_mat=T)
    prism(mb, ngon(0.052, 8, phase=OCT_PHASE), -0.015, 0.015, "YZ", G, bevel_w=0.004, bevel_mat=T)
    prism(mb, ngon(0.030, 8, phase=OCT_PHASE), -0.0185, 0.0185, "YZ", W, bevel_w=0.002, bevel_mat=T)
    return mb


# --------------------------------------------------------------------------
# SM_MRWall: centre origin, normal +X (faces the opponent), 0.16 x 1.6 x 2.0.
# Spawned at Z = 100 cm, so its base sits on the floor. Front-only light bars
# and base strip mark +X.
# --------------------------------------------------------------------------

def build_wall():
    mb = MeshBuilder("SM_MRWall", ["M_Wall", "M_Graphite", "M_Trim", "M_White"])
    E, G, T, W = (mb.m(s) for s in mb.slots)
    outer, _ = rounded_rect(0.790, 0.990, 0.100, 1)
    inner, _ = rounded_rect(0.660, 0.830, 0.060, 1, cy=0.030)
    ring_prism(mb, outer, inner, -0.062, 0.062, "YZ", G,
               outer_bevel=0.012, outer_mat=T, inner_bevel=0.014, inner_mat=E)
    field, _ = rounded_rect(0.690, 0.860, 0.080, 1, cy=0.030)
    prism(mb, field, -0.014, 0.014, "YZ", E)
    for z in (0.32, -0.26):
        box(mb, (0.060, 1.360, 0.050), G, center=(0.0, 0.0, z), bevel_w=0.008, bevel_mat=T)
        box(mb, (0.100, 0.160, 0.090), W, center=(0.0, 0.0, z), bevel_w=0.010, bevel_mat=T)
    bracket = [(0.80, 0.60), (0.80, 0.90), (0.70, 1.00), (0.44, 1.00), (0.44, 0.93),
               (0.671, 0.93), (0.73, 0.871), (0.73, 0.60)]
    for sy in (1.0, -1.0):
        for sz in (1.0, -1.0):
            prism(mb, [(sy * a, sz * b) for a, b in bracket], -0.080, 0.080, "YZ", W,
                  bevel_w=0.010, bevel_mat=T)
    prism(mb, [(-0.080, -1.000), (0.080, -1.000), (0.080, -0.940), (0.055, -0.900),
               (-0.055, -0.900), (-0.080, -0.940)], -0.450, 0.450, "XZ", G,
          bevel_w=0.010, bevel_mat=T)
    prism(mb, [(0.0704, -0.9321), (0.0772, -0.9279), (0.0647, -0.9079), (0.0579, -0.9121)],
          -0.400, 0.400, "XZ", E)
    for s in (1.0, -1.0):
        box(mb, (0.012, 0.030, 1.000), E, center=(0.064, s * 0.725, 0.050))
    return mb


# --------------------------------------------------------------------------
# SM_MRHead: origin = head centre (HMD pose), +X visor. Team visor (slot 0),
# white faceplate and crest; side pod cores and the rear module window carry
# the team colour to every inspector angle. Fits the r = 13 cm HeadHitbox.
# --------------------------------------------------------------------------

HEAD_C = (-0.004, 0.0, 0.0)
HEAD_R = (0.118, 0.110, 0.122)


def head_point(u, v, off):
    cu, su, cv, sv = math.cos(u), math.sin(u), math.cos(v), math.sin(v)
    return (HEAD_C[0] + (HEAD_R[0] + off) * cv * cu,
            HEAD_C[1] + (HEAD_R[1] + off) * cv * su,
            HEAD_C[2] + (HEAD_R[2] + off) * sv)


def build_head():
    mb = MeshBuilder("SM_MRHead", ["M_Blue", "M_Graphite", "M_Trim", "M_White"])
    B, G, T, W = (mb.m(s) for s in mb.slots)
    rad = math.radians
    us = [2.0 * math.pi * k / 16 for k in range(16)]
    rings = [[head_point(u, rad(v), 0.0) for u in us] for v in range(-80, 81, 20)]
    loft(mb, rings, G, caps=("fan", "fan"),
         poles=(head_point(0.0, -math.pi / 2, 0.0), head_point(0.0, math.pi / 2, 0.0)))
    # Wraparound visor, brow lip over it, white lower faceplate.
    shell_patch(mb, head_point, [rad(a) for a in linspace(-72, 72, 9)],
                [rad(a) for a in (-14, -2, 10, 22)], 0.010, -0.012, B, bevel_w=0.003, bevel_mat=T)
    # Every head vertex stays inside the r = 13 cm HeadHitbox sphere (checked as
    # an error in main). The sphere is convex and bevels only cut inward, so the
    # pre-bevel points below are the ones that matter; each keeps >= 1 mm margin.
    shell_patch(mb, head_point, [rad(a) for a in linspace(-80, 80, 11)],
                [rad(a) for a in (22, 28, 34)], 0.011, -0.010, G, bevel_w=0.003, bevel_mat=T)
    shell_patch(mb, head_point, [rad(a) for a in linspace(-58, 58, 9)],
                [rad(a) for a in (-52, -35, -18)], 0.008, -0.012, W, bevel_w=0.003, bevel_mat=T)
    # Crest along the crown (front to back).
    phis = [rad(a) for a in linspace(30, 165, 10)]
    crest = [(HEAD_C[0] + (HEAD_R[0] + 0.005) * math.cos(p), HEAD_C[2] + (HEAD_R[2] + 0.005) * math.sin(p))
             for p in phis]
    crest += [(HEAD_C[0] + (HEAD_R[0] - 0.016) * math.cos(p), HEAD_C[2] + (HEAD_R[2] - 0.016) * math.sin(p))
              for p in reversed(phis)]
    prism(mb, crest, -0.011, 0.011, "XZ", W, bevel_w=0.002, bevel_mat=T)
    # Side tracking pods with team cores, centred on the shell's side axis.
    for mirror in (None, MIRROR_Y):
        prism(mb, ngon(0.042, 8, HEAD_C[0], 0.0, OCT_PHASE), 0.094, 0.119, "XZ", G, mtx=mirror,
              bevel_w=0.004, bevel_mat=T)
        prism(mb, ngon(0.024, 8, HEAD_C[0], 0.0, OCT_PHASE), 0.115, 0.1255, "XZ", B, mtx=mirror,
              bevel_w=0.002, bevel_mat=T, bevel_on="hi")
    # Rear strap module: a plate conforming to the shell (6 mm proud) with a
    # flush team-colour window, so it reads from behind without leaving the sphere.
    shell_patch(mb, head_point, [rad(a) for a in (150, 165, 180, 195, 210)],
                [rad(a) for a in (-16, 0, 12, 28)], 0.006, -0.010, G, bevel_w=0.003, bevel_mat=T,
                face_mat=lambda a, b: B if a in (1, 2) and b == 1 else None)
    return mb


# --------------------------------------------------------------------------
# SM_MRBody: origin = torso centre (UE places it at head - 58 cm), +X front.
# Stays inside the existing body capsule (r 22 cm, half-height 50 cm).
# --------------------------------------------------------------------------

TORSO_N = 2.6
TORSO = [  # (z, half depth X, half width Y)
    (-0.400, 0.090, 0.120), (-0.340, 0.105, 0.138), (-0.200, 0.110, 0.142),
    (-0.050, 0.118, 0.150), (0.100, 0.135, 0.168), (0.240, 0.138, 0.172),
    (0.320, 0.125, 0.160), (0.370, 0.095, 0.118), (0.400, 0.058, 0.064),
]


def torso_dims(z):
    if z <= TORSO[0][0]:
        return TORSO[0][1], TORSO[0][2]
    for (z0, x0, y0), (z1, x1, y1) in zip(TORSO, TORSO[1:]):
        if z <= z1:
            t = (z - z0) / (z1 - z0)
            return x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
    return TORSO[-1][1], TORSO[-1][2]


def torso_point(u, z, off):
    hx, hy = torso_dims(z)
    c, s = math.cos(u), math.sin(u)
    e = 2.0 / TORSO_N
    return ((hx + off) * math.copysign(abs(c) ** e, c), (hy + off) * math.copysign(abs(s) ** e, s), z)


def build_body():
    mb = MeshBuilder("SM_MRBody", ["M_Blue", "M_Graphite", "M_Trim", "M_White"])
    B, G, T, W = (mb.m(s) for s in mb.slots)
    rad = math.radians
    us = [2.0 * math.pi * k / 20 for k in range(20)]
    loft(mb, [[torso_point(u, z, 0.0) for u in us] for z, _, _ in TORSO], G)
    # Chest plate and two abdominal plates.
    shell_patch(mb, torso_point, [rad(a) for a in linspace(-62, 62, 7)], linspace(0.02, 0.30, 5),
                0.024, -0.010, W, bevel_w=0.004, bevel_mat=T)
    for z0, z1 in ((-0.100, -0.030), (-0.190, -0.120)):
        shell_patch(mb, torso_point, [rad(a) for a in linspace(-56, 56, 7)], [z0, z1],
                    0.016, -0.010, G, bevel_w=0.003, bevel_mat=T)
    # Team belt visible from every side.
    closed_tube(mb, [[torso_point(u, z, off) for u in us]
                     for z, off in ((-0.305, 0.012), (-0.245, 0.012), (-0.245, -0.012), (-0.305, -0.012))], B)
    # Neck collar.
    def ring(hx, hy, z):
        return [(x, y, z) for x, y in superellipse(hx, hy, 2.4, 16)]
    closed_tube(mb, [ring(0.096, 0.116, 0.352), ring(0.080, 0.098, 0.398),
                     ring(0.062, 0.078, 0.398), ring(0.066, 0.082, 0.352)], T)
    # Shoulder pads with team stripes.
    for mirror in (None, MIRROR_Y):
        pad = Matrix.Translation((0.0, 0.158, 0.300)) @ rot("X", -22.0)
        if mirror is not None:
            pad = mirror @ pad
        box(mb, (0.130, 0.070, 0.050), G, mtx=pad, bevel_w=0.008, bevel_mat=T)
        box(mb, (0.104, 0.014, 0.008), B, center=(0.0, 0.016, 0.026), mtx=pad)
    # Back power unit with twin team cells and a white grab cap.
    box(mb, (0.086, 0.150, 0.300), G, center=(-0.153, 0.0, 0.075), bevel_w=0.012, bevel_mat=T)
    for s in (1.0, -1.0):
        box(mb, (0.008, 0.024, 0.200), B, center=(-0.196, s * 0.036, 0.075))
    box(mb, (0.060, 0.100, 0.024), W, center=(-0.150, 0.0, 0.236), bevel_w=0.005, bevel_mat=T)
    # Chest core: hex emitter in a trim ring.
    prism(mb, ngon(0.050, 6, 0.0, 0.150, math.pi / 2), 0.140, 0.180, "YZ", T,
          bevel_w=0.003, bevel_mat=G, bevel_on="hi")
    prism(mb, ngon(0.033, 6, 0.0, 0.150, math.pi / 2), 0.140, 0.194, "YZ", B,
          bevel_w=0.003, bevel_mat=T, bevel_on="hi")
    return mb


# --------------------------------------------------------------------------
# Asset table: the contract recorded in manifest.json.
# --------------------------------------------------------------------------

TEAM_SLOT0 = ("Slot 0 is the team colour. AMRFPSPawn calls MRFPS::Material(<visual>, Blue|Orange), "
              "which replaces material slot 0 only, so the rest of the gear keeps its palette.")

ASSETS = [
    dict(name="SM_MRPistol", build=build_pistol, group="Weapon", layout=(0.35, -0.25, 1.35),
         intended=(0.32, 0.065, 0.16),
         origin="Grip/hand attach point (0,0,0) on the bore axis; the grip hangs below it.",
         forward="+X = fire direction; bore runs along +X through the origin at Y=0, Z=0.",
         up="+Z = sight deck", slot0=TEAM_SLOT0,
         poi={"muzzle": (0.22, 0.0, 0.0)}, sockets={"SOCKET_Muzzle": (0.22, 0.0, 0.0)}),
    dict(name="SM_MRWall", build=build_wall, group="Abilities", layout=(0.0, -2.2, 1.0),
         intended=(0.16, 1.6, 2.0),
         origin="Box centre; matches AMRFPSBarrier box extent (8, 80, 100) cm. Spawned at Z=100 cm so the base meets the floor.",
         forward="+X = front/normal (faces away from the owner, toward the opponent). Light bars and base strip are front-only.",
         up="+Z", slot0="Slot 0 is M_Wall (MRFPS::Material(Visual, Wall) targets slot 0).",
         contain=("box", (0.08, 0.80, 1.00))),
    dict(name="SM_MRMiniShield", build=build_shield, group="Abilities", layout=(0.0, 1.3, 1.25),
         intended=(0.04, 0.44, 0.44),
         origin="Box centre; matches the MiniShield box extent (2, 22, 22) cm.",
         forward="+X = shield normal (pose forward from the palm).",
         up="+Z", slot0="Slot 0 is M_Shield (MRFPS::Material(ShieldVisual, Shield) targets slot 0).",
         contain=("box", (0.02, 0.22, 0.22))),
    dict(name="SM_MRHead", build=build_head, group="Avatar", layout=(0.0, 0.0, 1.70),
         intended=(0.26, 0.26, 0.26),
         origin="Head centre (HMD pose); HeadHitbox sphere r=13 cm is centred here and contains every vertex.",
         forward="+X = visor/facing direction.", up="+Z = crown", slot0=TEAM_SLOT0,
         contain=("sphere", 0.13)),
    dict(name="SM_MRBody", build=build_body, group="Avatar", layout=(0.0, 0.0, 1.12),
         intended=(0.40, 0.40, 0.80),
         origin="Torso centre; BodyHitbox capsule (r 22 cm, half-height 50 cm) is centred here (head - 58 cm).",
         forward="+X = chest/front.", up="+Z = neck", slot0=TEAM_SLOT0,
         contain=("capsule", (0.22, 0.50))),
    dict(name="SM_MRHand", build=build_hand, group="Avatar", layout=(0.30, 0.30, 1.20),
         intended=(0.18, 0.10, 0.055),
         origin="Wrist centre (0,0,0).",
         forward="+X = fingers, +Z = dorsal (back of hand), palm faces -Z. LEFT hand: thumb on Blender -Y, which is UE +Y.",
         up="+Z = dorsal", slot0=TEAM_SLOT0,
         poi={"palm_emitter": (0.054, 0.002, -0.0195)}),
]


def reference_mesh(name, kind, dims):
    """Wireframe hitbox references kept in the .blend only (never exported)."""
    mb = MeshBuilder(name, [])
    if kind == "box":
        box(mb, [d * 2.0 for d in dims], 0)
    else:
        r, hh = (dims, dims) if kind == "sphere" else dims
        seg = hh - r
        rings = []
        for c, angles in ((-seg, (-67.5, -45.0, -22.5, 0.0)), (seg, (0.0, 22.5, 45.0, 67.5))):
            for a in angles:
                if kind == "sphere" and c > 0 and a == 0.0:
                    continue
                v = math.radians(a)
                rings.append([(r * math.cos(v) * math.cos(t), r * math.cos(v) * math.sin(t), c + r * math.sin(v))
                              for t in (2.0 * math.pi * k / 16 for k in range(16))])
        loft(mb, rings, 0, caps=("fan", "fan"), poles=((0.0, 0.0, -hh), (0.0, 0.0, hh)))
    mesh = bpy.data.meshes.new(name)
    mb.bm.to_mesh(mesh)
    mb.bm.free()
    return mesh


# --------------------------------------------------------------------------
# Measurement and validation (on the evaluated, triangulated mesh)
# --------------------------------------------------------------------------

def mesh_stats(obj, slots):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(depsgraph)
    me = ev.to_mesh()
    try:
        me.calc_loop_triangles()
        cos = [v.co.copy() for v in me.vertices]
        tris = [tuple(t.vertices) for t in me.loop_triangles]
        tri_mats = [t.material_index for t in me.loop_triangles]
        used_edges = set()
        for p in me.polygons:
            used_edges.update(p.edge_keys)
        loose_edges = sum(1 for e in me.edges if e.key not in used_edges)
        edge_use = {}
        for p in me.polygons:
            for k in p.edge_keys:
                edge_use[k] = edge_use.get(k, 0) + 1
        non_manifold = sum(1 for c in edge_use.values() if c != 2)
    finally:
        ev.to_mesh_clear()

    used_verts = {i for t in tris for i in t}
    parent = list(range(len(cos)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for a, b, c in tris:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    volume = {}
    zero_area = 0
    for a, b, c in tris:
        pa, pb, pc = cos[a], cos[b], cos[c]
        if (pb - pa).cross(pc - pa).length < 2e-10:
            zero_area += 1
        r = find(a)
        volume[r] = volume.get(r, 0.0) + pa.dot(pb.cross(pc)) / 6.0
    lo = Vector((min(c.x for c in cos), min(c.y for c in cos), min(c.z for c in cos)))
    hi = Vector((max(c.x for c in cos), max(c.y for c in cos), max(c.z for c in cos)))

    digest = hashlib.sha256()
    for c in cos:
        digest.update(("%.5f %.5f %.5f;" % (round(c.x, 5) + 0.0, round(c.y, 5) + 0.0, round(c.z, 5) + 0.0)).encode())
    for t, m in zip(tris, tri_mats):
        digest.update(("%d %d %d %d;" % (t[0], t[1], t[2], m)).encode())

    per_slot = {s: 0 for s in slots}
    for m in tri_mats:
        per_slot[slots[m]] += 1
    return dict(
        triangles=len(tris), vertices=len(cos), cos=cos,
        bounds_min=lo, bounds_max=hi,
        triangles_per_slot=per_slot,
        validation=dict(
            loose_vertices=len(cos) - len(used_verts), loose_edges=loose_edges,
            non_manifold_edges=non_manifold, zero_area_triangles=zero_area,
            closed_parts=len(volume), inverted_parts=sum(1 for v in volume.values() if v <= 0.0),
        ),
        geometry_sha256=digest.hexdigest(),
    )


def containment(spec, cos):
    kind, dims = spec
    worst = -1.0
    for c in cos:
        if kind == "box":
            d = max(abs(c.x) - dims[0], abs(c.y) - dims[1], abs(c.z) - dims[2])
        elif kind == "sphere":
            d = c.length - dims
        else:
            r, hh = dims
            zc = max(-(hh - r), min(hh - r, c.z))
            d = math.sqrt(c.x * c.x + c.y * c.y + (c.z - zc) ** 2) - r
        worst = max(worst, d)
    return worst


# --------------------------------------------------------------------------
# Scene objects
# --------------------------------------------------------------------------

def new_collection(name, parent):
    coll = bpy.data.collections.new(name)
    parent.children.link(coll)
    return coll


def build_assets(scene, mats):
    root = new_collection("MRFPS_Assets", scene.collection)
    refs = new_collection("MRFPS_HitboxReference", scene.collection)
    groups = {}
    records = []
    for spec in ASSETS:
        log("building " + spec["name"])
        mb = spec["build"]()
        spec["slots"] = list(mb.slots)
        mesh = finalize_mesh(mb, mats)
        obj = bpy.data.objects.new(spec["name"], mesh)
        group = groups.get(spec["group"])
        if group is None:
            group = groups[spec["group"]] = new_collection(spec["group"], root)
        group.objects.link(obj)
        obj.location = spec["layout"]  # display only; exports are written at the origin
        tri = obj.modifiers.new("Triangulate", "TRIANGULATE")
        tri.quad_method = "BEAUTY"
        tri.ngon_method = "BEAUTY"
        tri.min_vertices = 4
        obj["mrfps_origin"] = spec["origin"]
        obj["mrfps_forward"] = spec["forward"]
        for sock, loc in spec.get("sockets", {}).items():
            empty = bpy.data.objects.new(sock, None)
            empty.empty_display_type = "ARROWS"
            empty.empty_display_size = 0.04
            empty.parent = obj
            empty.location = loc
            group.objects.link(empty)
        if "contain" in spec:
            kind, dims = spec["contain"]
            ref = bpy.data.objects.new("REF_%s_Hitbox" % spec["name"][3:], reference_mesh(
                "REF_%s_Hitbox" % spec["name"][3:], kind, dims))
            ref.display_type = "WIRE"
            ref.hide_render = True
            ref.parent = obj
            refs.objects.link(ref)
        records.append((spec, obj))
    bpy.context.view_layer.update()
    return records


# --------------------------------------------------------------------------
# Export (FBX for UE, GLB for quick inspection) and FBX round-trip check
# --------------------------------------------------------------------------

def ensure_io_addons():
    """--factory-startup enables these by default; enable them if a build did not."""
    import addon_utils
    for op_type, module in (("EXPORT_SCENE_OT_fbx", "io_scene_fbx"), ("EXPORT_SCENE_OT_gltf", "io_scene_gltf2")):
        if not hasattr(bpy.types, op_type):
            try:
                addon_utils.enable(module, default_set=False)
            except Exception as exc:
                log("could not enable %s: %s" % (module, exc))


def op_kwargs(op, wanted):
    """Keep only options this Blender build knows, so API drift cannot abort."""
    try:
        props = set(op.get_rna_type().properties.keys())
    except Exception:  # pragma: no cover - very old/new API
        return dict(wanted), []
    return ({k: v for k, v in wanted.items() if k in props},
            sorted(k for k in wanted if k not in props))


def select_only(objs):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]


def strip_suffix(name):
    return re.sub(r"\.\d{3}$", "", name)


FBX_OPTIONS = dict(
    check_existing=False, use_selection=True, use_mesh_modifiers=True, mesh_smooth_type="FACE",
    use_tspace=False, use_custom_props=False, add_leaf_bones=False, bake_anim=False,
    apply_unit_scale=True, apply_scale_options="FBX_SCALE_UNITS", global_scale=1.0,
    axis_forward="-Z", axis_up="Y", use_space_transform=True, bake_space_transform=False,
    path_mode="AUTO", embed_textures=False, colors_type="NONE",
)

GLB_OPTIONS = dict(
    check_existing=False, export_format="GLB", use_selection=True, export_apply=True,
    export_yup=True, export_cameras=False, export_lights=False, export_animations=False,
    export_extras=False, export_materials="EXPORT", export_texcoords=True, export_normals=True,
)


def export_asset(spec, obj, errors):
    name = spec["name"]
    paths = {"fbx": os.path.join(EXPORT_DIR, name + ".fbx"), "glb": os.path.join(EXPORT_DIR, name + ".glb")}
    for p in paths.values():
        if os.path.isfile(p):
            os.remove(p)  # exact generated file only
    result = {"fbx": None, "glb": None, "dropped_options": {}}
    saved = obj.location.copy()
    obj.location = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()
    sockets = [c for c in obj.children if c.name.startswith("SOCKET_")]
    select_only([obj] + sockets)
    try:
        kw, dropped = op_kwargs(bpy.ops.export_scene.fbx, dict(
            FBX_OPTIONS, filepath=paths["fbx"],
            object_types={"MESH", "EMPTY"} if sockets else {"MESH"}))
        result["dropped_options"]["fbx"] = dropped
        if "FINISHED" in bpy.ops.export_scene.fbx(**kw) and os.path.isfile(paths["fbx"]):
            result["fbx"] = "Exports/%s.fbx" % name
        else:
            errors.append("%s: FBX export did not finish" % name)
    except Exception as exc:
        errors.append("%s: FBX export failed: %s" % (name, exc))
    select_only([obj] + sockets)
    try:
        kw, dropped = op_kwargs(bpy.ops.export_scene.gltf, dict(GLB_OPTIONS, filepath=paths["glb"]))
        result["dropped_options"]["glb"] = dropped
        if "FINISHED" in bpy.ops.export_scene.gltf(**kw) and os.path.isfile(paths["glb"]):
            result["glb"] = "Exports/%s.glb" % name
    except Exception as exc:  # GLB is a convenience format; FBX is the UE source
        log("%s: GLB export skipped: %s" % (name, exc))
    obj.location = saved
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    bpy.context.view_layer.update()
    return result


def fbx_roundtrip(path, spec, stats):
    """Re-import the FBX into Blender and compare bounds/slots, then delete it all."""
    kinds = ("objects", "meshes", "materials", "actions", "images", "collections")
    before = {k: {i.name for i in getattr(bpy.data, k)} for k in kinds}
    out = {"ok": False}
    try:
        bpy.ops.import_scene.fbx(filepath=path)
        new_objs = [o for o in bpy.data.objects if o.name not in before["objects"]]
        meshes = [o for o in new_objs if o.type == "MESH"]
        bpy.context.view_layer.update()
        pts = [o.matrix_world @ v.co for o in meshes for v in o.data.vertices]
        lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
        hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
        err = max(max(abs(a - b) for a, b in zip(lo, stats["bounds_min"])),
                  max(abs(a - b) for a, b in zip(hi, stats["bounds_max"])))
        slots = [strip_suffix(s.material.name) for o in meshes[:1] for s in o.material_slots if s.material]
        tris = sum(len(p.vertices) - 2 for o in meshes for p in o.data.polygons)
        empties = {strip_suffix(o.name): rnd(o.matrix_world.translation) for o in new_objs if o.type == "EMPTY"}
        expected_slots = [m for m in spec["slots"]]
        out = dict(
            ok=(len(meshes) == 1 and err < 2e-4 and sorted(slots) == sorted(expected_slots)
                and tris == stats["triangles"]),
            mesh_objects=len(meshes), triangles=tris, max_bounds_error_m=round(err, 7),
            material_slots=slots, slot_order_preserved=(slots == expected_slots), empties=empties,
        )
    except Exception as exc:
        out = {"ok": False, "error": str(exc)}
    finally:
        for k in kinds:
            ids = [i for i in getattr(bpy.data, k) if i.name not in before[k]]
            if ids:
                bpy.data.batch_remove(ids)
    return out


# --------------------------------------------------------------------------
# Preview board: orthographic 16 x 9 unit frame. Display objects are linked
# duplicates of the real meshes (same mesh data), uniformly scaled to fit
# their cell; captions print the measured, unscaled numbers.
# --------------------------------------------------------------------------

# name: (centre x, centre z, width, height, yaw deg, tilt deg, slot-0 shown, accent)
BOARD_CELLS = {
    "SM_MRPistol":     (-2.0, 2.25, 4.0, 4.5, -16.0, 10.0, "M_Blue", "blue"),
    "SM_MRHand":       (2.0, 2.25, 4.0, 4.5, -18.0, 52.0, "M_Blue", "blue"),
    "SM_MRHead":       (-6.0, -2.25, 4.0, 4.5, -48.0, 10.0, "M_Orange", "orange"),
    "SM_MRBody":       (-2.0, -2.25, 4.0, 4.5, -50.0, 8.0, "M_Orange", "orange"),
    "SM_MRMiniShield": (2.0, -2.25, 4.0, 4.5, -60.0, 8.0, None, "shield"),
    "SM_MRWall":       (6.0, 0.0, 4.0, 9.0, -42.0, 6.0, None, "wall"),
}
LEGEND_CELL = (-6.0, 2.25, 4.0, 4.5)

BOARD_ROLE = {
    "SM_MRPistol": "origin grip  |  muzzle +X 0.22 m",
    "SM_MRHand": "origin wrist  |  +X fingers, +Z dorsal",
    "SM_MRMiniShield": "origin centre  |  normal +X",
    "SM_MRHead": "origin head centre  |  +X visor",
    "SM_MRBody": "origin torso centre  |  +X front",
    "SM_MRWall": "origin centre  |  normal +X",
}
BOARD_NOTE = {
    "SM_MRPistol": "slot 0 team colour (blue shown)",
    "SM_MRHand": "left hand  |  slot 0 team colour",
    "SM_MRHead": "inside r 13 cm hit sphere  |  orange shown",
    "SM_MRBody": "inside hit capsule  |  orange shown",
    "SM_MRMiniShield": "fills its 4 x 44 x 44 cm hit box",
    "SM_MRWall": "fills its 16 x 160 x 200 cm hit box",
}
ACCENTS = {
    "blue": (0.040, 0.550, 1.000), "orange": (1.000, 0.240, 0.035),
    "wall": (0.080, 0.400, 0.650), "shield": (0.100, 0.950, 0.600),
}


def card_material():
    mat = bpy.data.materials.new("PV_Card")
    tree = _node_tree(mat)
    nodes, links = tree.nodes, tree.links
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    _set_input(bsdf, "Roughness", 0.85)
    coord = nodes.new("ShaderNodeTexCoord")
    sep = nodes.new("ShaderNodeSeparateXYZ")
    links.new(coord.outputs["Object"], sep.inputs[0])

    def grid_line(axis_socket):
        mul = nodes.new("ShaderNodeMath")
        mul.operation = "MULTIPLY"
        mul.inputs[1].default_value = 2.0  # 0.5 unit grid, like the arena floor
        links.new(axis_socket, mul.inputs[0])
        frac = nodes.new("ShaderNodeMath")
        frac.operation = "FRACT"
        links.new(mul.outputs[0], frac.inputs[0])
        less = nodes.new("ShaderNodeMath")
        less.operation = "LESS_THAN"
        less.inputs[1].default_value = 0.025
        links.new(frac.outputs[0], less.inputs[0])
        return less.outputs[0]

    both = nodes.new("ShaderNodeMath")
    both.operation = "MAXIMUM"
    links.new(grid_line(sep.outputs["X"]), both.inputs[0])
    links.new(grid_line(sep.outputs["Z"]), both.inputs[1])
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.017, 0.021, 0.029, 1.0)
    ramp.color_ramp.elements[1].color = (0.034, 0.048, 0.068, 1.0)
    links.new(both.outputs[0], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    mat.diffuse_color = (0.02, 0.025, 0.035, 1.0)
    return mat


def _static_object(coll, name, mb, mat):
    mesh = bpy.data.meshes.new(name)
    mb.bm.to_mesh(mesh)
    mb.bm.free()
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    coll.objects.link(obj)
    return obj


def add_card(coll, name, cx, cz, w, h, y, card_mat, accent_mat):
    mb = MeshBuilder(name, [])
    pts, _ = rounded_rect(w * 0.5, h * 0.5, 0.12, 3, cx=cx, cy=cz)
    prism(mb, pts, y, y + 0.04, "XZ", 0)
    _static_object(coll, name, mb, card_mat)
    if accent_mat is not None:
        mb = MeshBuilder(name + "_Accent", [])
        box(mb, (w - 0.6, 0.02, 0.035), 0, center=(cx, y - 0.012, cz + h * 0.5 - 0.17))
        _static_object(coll, name + "_Accent", mb, accent_mat)


def add_text(coll, name, body, size, x, z, y, mat, align="CENTER"):
    curve = bpy.data.curves.new(name, "FONT")
    curve.body = body
    curve.size = size
    curve.align_x = align
    curve.align_y = "CENTER"
    curve.materials.append(mat)
    obj = bpy.data.objects.new(name, curve)
    obj.location = (x, y, z)
    obj.rotation_euler = (math.radians(90.0), 0.0, 0.0)  # face the -Y camera
    coll.objects.link(obj)
    return obj


def add_sun(coll, name, direction, strength, color, angle_deg, shadow):
    light = bpy.data.lights.new(name, "SUN")
    light.energy = strength
    light.color = color
    light.angle = math.radians(angle_deg)
    light.use_shadow = shadow
    obj = bpy.data.objects.new(name, light)
    obj.rotation_euler = Vector(direction).normalized().to_track_quat("-Z", "Y").to_euler()
    coll.objects.link(obj)


def display_instance(coll, obj, stats, cell, mats):
    cx, cz, w, h, yaw, tilt, shown, _ = cell
    R = rot("X", tilt) @ rot("Z", yaw)
    pts = [R @ c for c in stats["cos"]]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    top, bottom = cz + h * 0.5 - 0.40, cz - h * 0.5 + 1.30
    s = min((w - 1.0) / (hi.x - lo.x), (top - bottom) / (hi.z - lo.z))
    mid = (lo + hi) * 0.5
    target = Vector((cx, 0.0, (top + bottom) * 0.5))
    inst = bpy.data.objects.new("PV_" + obj.name, obj.data)  # linked mesh data
    coll.objects.link(inst)
    inst.matrix_world = Matrix.Translation(target - mid * s) @ Matrix.Scale(s, 4) @ R
    if shown and shown != obj.data.materials[0].name:
        inst.material_slots[0].link = "OBJECT"
        inst.material_slots[0].material = mats[shown]
    return (hi.y - mid.y) * s


def hide_sources_from_render(records):
    """After export: keep the editable source meshes (viewport-visible) but out of
    renders, so the board shows only its own display copies, cards and text."""
    for _, obj in records:
        obj.hide_render = True
        for child in obj.children:
            child.hide_render = True
    for name in ("MRFPS_Assets", "MRFPS_HitboxReference"):
        coll = bpy.data.collections.get(name)
        if coll is not None:
            coll.hide_render = True


def board_render_leaks(scene, board):
    """Renderable objects that are not part of the board collection."""
    kinds = {"MESH", "CURVE", "FONT", "SURFACE", "META", "VOLUME", "POINTCLOUD", "CURVES"}
    return sorted(o.name for o in scene.objects
                  if o.type in kinds and not o.hide_render and board not in o.users_collection)


def build_board(scene, records, stats_by_name, mats, total_tris, engine_label):
    coll = new_collection("MRFPS_PreviewBoard", scene.collection)
    cam_data = bpy.data.cameras.new("BoardCamera")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 16.0
    cam_data.clip_start = 0.1
    cam_data.clip_end = 100.0
    cam = bpy.data.objects.new("BoardCamera", cam_data)
    cam.location = (0.0, -30.0, 0.0)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    coll.objects.link(cam)
    scene.camera = cam

    add_sun(coll, "Key", (0.42, 0.80, -0.43), 2.6, (1.0, 0.97, 0.93), 14.0, True)
    add_sun(coll, "Fill", (-0.70, 0.55, -0.20), 0.55, (0.75, 0.85, 1.0), 30.0, False)
    add_sun(coll, "Rim", (0.30, -0.80, -0.50), 1.5, (0.60, 0.85, 1.0), 10.0, False)

    world = bpy.data.worlds.new("BoardWorld")
    scene.world = world
    world.color = (0.010, 0.013, 0.020)
    tree = _node_tree(world)
    if tree is not None:
        bg = next((n for n in tree.nodes if n.type == "BACKGROUND"), None)
        if bg is None:
            bg = tree.nodes.new("ShaderNodeBackground")
            out = (next((n for n in tree.nodes if n.type == "OUTPUT_WORLD"), None)
                   or tree.nodes.new("ShaderNodeOutputWorld"))
            tree.links.new(bg.outputs["Background"], out.inputs["Surface"])
        bg.inputs["Color"].default_value = (0.010, 0.013, 0.020, 1.0)
        bg.inputs["Strength"].default_value = 1.0

    card_mat = card_material()
    backdrop = MeshBuilder("PV_Backdrop", [])
    box(backdrop, (20.0, 0.1, 12.0), 0, center=(0.0, 8.0, 0.0))
    _static_object(coll, "PV_Backdrop", backdrop, emission_material("PV_Backdrop", (0.006, 0.008, 0.012)))
    white = emission_material("PV_TextWhite", (0.92, 0.95, 1.00))
    muted = emission_material("PV_TextMuted", (0.40, 0.48, 0.58))
    text_accent = {"blue": emission_material("PV_TextBlue", (0.25, 0.62, 1.00)),
                   "orange": emission_material("PV_TextOrange", (1.00, 0.45, 0.15)),
                   "wall": emission_material("PV_TextWall", (0.30, 0.65, 0.95)),
                   "shield": emission_material("PV_TextShield", (0.30, 0.95, 0.70))}
    accent = {k: emission_material("PV_Accent_" + k, c, 1.3) for k, c in ACCENTS.items()}

    for spec, obj in records:
        name = spec["name"]
        cell = BOARD_CELLS[name]
        cx, cz, w, h = cell[:4]
        stats = stats_by_name[name]
        back = display_instance(coll, obj, stats, cell, mats)
        card_y = back + 0.20
        add_card(coll, "PV_Card_" + name, cx, cz, w - 0.2, h - 0.2, card_y, card_mat, accent[cell[7]])
        size = stats["bounds_max"] - stats["bounds_min"]
        base, ty = cz - h * 0.5, card_y - 0.03
        add_text(coll, "PV_Name_" + name, name, 0.30, cx, base + 1.06, ty, white)
        add_text(coll, "PV_Dims_" + name, "%.3f x %.3f x %.3f m  |  %d tris" % (
            size.x, size.y, size.z, stats["triangles"]), 0.155, cx, base + 0.76, ty, muted)
        add_text(coll, "PV_Role_" + name, BOARD_ROLE[name], 0.155, cx, base + 0.52, ty, text_accent[cell[7]])
        add_text(coll, "PV_Note_" + name, BOARD_NOTE[name], 0.14, cx, base + 0.30, ty, muted)

    lx, lz, lw, lh = LEGEND_CELL
    add_card(coll, "PV_Card_Legend", lx, lz, lw - 0.2, lh - 0.2, 0.5, card_mat, accent["blue"])
    left, ty = lx - lw * 0.5 + 0.45, 0.47
    add_text(coll, "PV_Title", "MRFPS", 0.80, left, 3.78, ty, white, "LEFT")
    add_text(coll, "PV_Subtitle", "MR TRAINING GEAR", 0.26, left, 3.12, ty, text_accent["blue"], "LEFT")
    add_text(coll, "PV_Totals", "6 static meshes  |  %d tris total" % total_tris, 0.17, left, 2.78, ty, muted, "LEFT")
    for i, name in enumerate(PALETTE):
        x = lx - 1.35 + 0.9 * (i % 4)
        z = 2.05 if i < 4 else 1.05
        chip = MeshBuilder("PV_Swatch_" + name, [])
        box(chip, (0.62, 0.10, 0.40), 0, center=(x, 0.40, z), bevel_w=0.04, bevel_mat=0)
        _static_object(coll, "PV_Swatch_" + name, chip, mats[name])
        add_text(coll, "PV_SwatchLabel_" + name, name[2:], 0.16, x, z - 0.42, ty, muted)
    add_text(coll, "PV_Footer", "Blender %s | %s | create_assets.py" % (
        bpy.app.version_string, engine_label), 0.13, left, 0.30, ty, muted, "LEFT")
    return coll


def configure_render(scene, engine, samples):
    r = scene.render
    r.resolution_x, r.resolution_y, r.resolution_percentage = 1280, 720, 100
    r.threads_mode = "FIXED"
    r.threads = 2
    r.film_transparent = False
    r.use_file_extension = True
    r.filepath = BOARD_PATH
    img = r.image_settings
    img.file_format = "PNG"
    img.color_mode = "RGB"
    img.color_depth = "8"
    img.compression = 15
    used = None
    if engine == "eevee":
        for eid in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
            try:
                r.engine = eid
                used = eid
                break
            except TypeError:
                continue
        if used:
            ev = scene.eevee
            for attr, value in (("taa_render_samples", samples or 64), ("use_shadows", True),
                                ("use_raytracing", False), ("use_gtao", True),
                                ("use_soft_shadows", True)):
                if hasattr(ev, attr):
                    try:
                        setattr(ev, attr, value)
                    except (TypeError, AttributeError):
                        pass
    if used is None:
        r.engine = "CYCLES"
        used = "CYCLES"
        cyc = scene.cycles
        cyc.device = "CPU"
        cyc.samples = samples or 48
        for attr, value in (("use_denoising", True), ("use_adaptive_sampling", True),
                            ("max_bounces", 4), ("diffuse_bounces", 2), ("glossy_bounces", 2),
                            ("transmission_bounces", 2)):
            if hasattr(cyc, attr):
                setattr(cyc, attr, value)
    vs = scene.view_settings
    for attr, value in (("view_transform", "Standard"), ("look", "None"), ("exposure", 0.0), ("gamma", 1.0)):
        try:
            setattr(vs, attr, value)
        except (TypeError, AttributeError):
            pass
    label = "EEVEE" if used.startswith("BLENDER_EEVEE") else "Cycles CPU"
    return used, label


# --------------------------------------------------------------------------
# Manifest and entry point
# --------------------------------------------------------------------------

UE_IMPORT = {
    "source_format": "FBX (Exports/<Name>.fbx); GLB is for inspection only",
    "axis": "Blender (x, y, z) m -> UE (100x, -100y, 100z) cm. +X forward and +Z up are preserved; "
            "UE's Y flip converts right-handed to left-handed without mirroring the shape.",
    "settings": {
        "Import As": "Static Mesh (no skeletal, no animation)",
        "Import Uniform Scale": 1.0,
        "Convert Scene": True,
        "Force Front X Axis": False,
        "Convert Scene Unit": True,
        "Import Translation/Rotation": "zero",
        "Combine Meshes": True,
        "Normal Import Method": "Import Normals",
        "Generate Lightmap UVs": False,
        "Auto Generate Collision": False,
        "Build Nanite": False,
        "Import Sockets": "on (SM_MRPistol carries SOCKET_Muzzle)",
        "Materials": "Do not create materials; resolve slot names to /Game/Materials/M_<Name>",
    },
    "verify_after_import": "Static Mesh Editor bounds should equal manifest bounds_m x 100 (e.g. wall 16 x 160 x 200 cm).",
}

UE_EXISTING_MATERIALS = ("M_Blue", "M_Orange", "M_Wall", "M_Shield", "M_Tracer")


def write_manifest(data):
    tmp = MANIFEST_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")
    os.replace(tmp, MANIFEST_PATH)


def jsonable(value):
    if isinstance(value, (set, frozenset, tuple)):
        return sorted(value) if isinstance(value, (set, frozenset)) else list(value)
    return value


def main():
    args = parse_args()
    for d in (GEN_DIR, EXPORT_DIR, PREVIEW_DIR):
        os.makedirs(d, exist_ok=True)
    errors, warnings = [], []
    log("Blender %s, output %s" % (bpy.app.version_string, GEN_DIR))

    ensure_io_addons()
    scene = reset_scene()
    mats = make_materials()
    records = build_assets(scene, mats)

    entries, stats_by_name = [], {}
    for spec, obj in records:
        name = spec["name"]
        stats = mesh_stats(obj, spec["slots"])
        stats_by_name[name] = stats
        val = stats["validation"]
        for key in ("loose_vertices", "loose_edges", "inverted_parts"):
            if val[key]:
                errors.append("%s: %s = %d" % (name, key, val[key]))
        for key in ("non_manifold_edges", "zero_area_triangles"):
            if val[key]:
                warnings.append("%s: %s = %d" % (name, key, val[key]))
        if stats["triangles"] > 6000:
            errors.append("%s: %d triangles exceeds 6000" % (name, stats["triangles"]))
        fit = None
        if "contain" in spec:
            kind, dims = spec["contain"]
            excess = containment(spec["contain"], stats["cos"])
            fit = dict(kind=kind, dims_m=jsonable(dims), max_excess_m=round(excess, 5) + 0.0,
                       inside=excess <= 1e-5)
            if excess > 1e-5:  # every visual must stay inside its gameplay volume
                errors.append("%s: extends %.1f mm beyond its %s hit volume" % (name, excess * 1000.0, kind))
        muzzle = spec.get("poi", {}).get("muzzle")
        if muzzle and abs(stats["bounds_max"].x - muzzle[0]) > 0.001:
            warnings.append("%s: front face at X=%.4f, muzzle contract is %.3f" % (name, stats["bounds_max"].x, muzzle[0]))

        export = export_asset(spec, obj, errors)
        if export["fbx"]:
            rt = fbx_roundtrip(os.path.join(GEN_DIR, export["fbx"]), spec, stats)
            if not rt.get("ok"):
                errors.append("%s: FBX round-trip mismatch: %s" % (name, rt))
        else:
            rt = {"ok": False, "error": "FBX missing"}
        lo, hi = stats["bounds_min"], stats["bounds_max"]
        entries.append(dict(
            name=name,
            files=dict(fbx=export["fbx"], glb=export["glb"]),
            triangles=stats["triangles"],
            vertices=stats["vertices"],
            triangles_per_slot=stats["triangles_per_slot"],
            material_slots=spec["slots"],
            slot0=spec["slot0"],
            bounds_m=dict(min=rnd(lo), max=rnd(hi), size=rnd(hi - lo)),
            intended_size_m=list(spec["intended"]),
            attach_origin=spec["origin"],
            forward=spec["forward"],
            up=spec["up"],
            points_of_interest_m={k: rnd(v) for k, v in spec.get("poi", {}).items()},
            sockets={k: dict(blender_m=rnd(v), ue_cm=rnd((v[0] * 100.0, -v[1] * 100.0, v[2] * 100.0), 3))
                     for k, v in spec.get("sockets", {}).items()},
            hit_volume_fit=fit,
            validation=val,
            geometry_sha256=stats["geometry_sha256"],
            fbx_roundtrip=rt,
            export_options_unsupported_by_this_blender=export["dropped_options"],
        ))
        log("%-16s %5d tris  size %s m" % (name, stats["triangles"], rnd(hi - lo, 4)))

    total = sum(e["triangles"] for e in entries)
    if total > 25000:
        errors.append("total %d triangles exceeds 25000" % total)

    hide_sources_from_render(records)
    engine, engine_label = configure_render(scene, args.engine, args.samples)
    board = build_board(scene, records, stats_by_name, mats, total, engine_label)
    leaks = board_render_leaks(scene, board)
    if leaks:
        errors.append("objects outside MRFPS_PreviewBoard would render on the board: %s" % ", ".join(leaks))

    try:
        kw, _ = op_kwargs(bpy.ops.wm.save_as_mainfile, dict(
            filepath=BLEND_PATH, check_existing=False, compress=False, relative_remap=True))
        bpy.ops.wm.save_as_mainfile(**kw)
        if os.path.isfile(BLEND_PATH + "1"):
            os.remove(BLEND_PATH + "1")  # Blender's backup of the previous run
    except Exception as exc:
        errors.append("saving .blend failed: %s" % exc)

    preview = dict(path="../Preview/AssetBoard.png", resolution=[1280, 720], engine=engine,
                   samples=(scene.eevee.taa_render_samples if engine.startswith("BLENDER_EEVEE")
                            else scene.cycles.samples),
                   threads=scene.render.threads,
                   render_set="MRFPS_PreviewBoard only (source assets hide_render after export)",
                   render_leaks=leaks,
                   status="skipped (--no-render)" if args.no_render else "pending")
    manifest = dict(
        generator="Art/create_assets.py",
        generator_version=GENERATOR_VERSION,
        blender_version=bpy.app.version_string,
        units="metres (Blender and this manifest); UE centimetres = metres x 100",
        coordinates=dict(
            blender="+X forward, +Y left, +Z up (right-handed)",
            ue="+X forward, +Y right, +Z up (left-handed)",
        ),
        blend="MRFPS_Assets.blend",
        fbx_export_options={k: jsonable(v) for k, v in FBX_OPTIONS.items()},
        ue_import=UE_IMPORT,
        materials={n: dict(preview_base_color=list(p["base"]),
                           preview_emission_color=list(p.get("emit_color", (0.0, 0.0, 0.0))),
                           preview_emission_strength=p["emit"],
                           ue_unlit_emissive_color=list(p["ue"]),
                           already_created_by_MRFPSSetupCommandlet=n in UE_EXISTING_MATERIALS)
                   for n, p in PALETTE.items()},
        assets=entries,
        totals=dict(triangles=total, budget_per_asset=6000, budget_total=25000),
        preview=preview,
        errors=errors,
        warnings=warnings,
    )
    write_manifest(manifest)

    if not args.no_render:
        if os.path.isfile(BOARD_PATH):
            os.remove(BOARD_PATH)  # exact generated file only
        log("rendering AssetBoard.png with %s (threads=2)" % engine_label)
        try:
            bpy.ops.render.render(write_still=True)
            if os.path.isfile(BOARD_PATH):
                preview["status"] = "rendered"
            else:
                preview["status"] = "render finished but AssetBoard.png is missing"
                errors.append(preview["status"])
        except Exception as exc:
            preview["status"] = "failed: %s" % exc
            errors.append("render failed: %s" % exc)
        write_manifest(manifest)

    log("total %d triangles; %d error(s), %d warning(s)" % (total, len(errors), len(warnings)))
    for w in warnings:
        log("WARNING " + w)
    for e in errors:
        log("ERROR " + e)
    if errors:
        sys.exit(1)
    log("done")


if __name__ == "__main__":
    main()
