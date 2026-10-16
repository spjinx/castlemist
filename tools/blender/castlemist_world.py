"""castlemist world export -> Blender / FBX (sub-project 4).

Headless:
    blender -b --factory-startup -P castlemist_world.py -- <command> [<command> ...]

Commands run in order, in one Blender session:
    --to-fbx <exportDir>          every .glb under <exportDir> (probe.glb, cells/*.glb,
                                  models/*/*.glb) to a .fbx beside it; files whose .fbx
                                  is newer than the .glb are skipped. Prints
                                  TO_FBX converted=<n> skipped=<n> failed=<n> seconds=<s>;
                                  a file that fails is reported and the rest still run.
                                  Each .fbx is written to a temporary file and renamed.
                                  Then the manifest's fbx fields are set to the .fbx
                                  files present (TO_FBX_MANIFEST fbxSet=<n>), as
                                  gw2dat_cli world-fbx does, so Unity can import
    --import <exportDir> [--no-colliders] [--no-dark-decal] [--scale 1.0] [--verify]
                         [--save <file.blend>]
                                  builds the scene (spec D6 "Blender"), see import_world();
                                  every foliage record (world-foliage D8) becomes a point of
                                  one geometry-nodes object per (cell, model)
    --check-probe <file.glb|fbx>  imports the frame probe into an empty scene and prints
                                  PROBE east=(x,y,z) north=(..) up=(..) negScale=<0|1> OK
                                  or ... FAIL <axis>
    --check-skinned-probe <file.glb|fbx>
                                  the skinned probe (tests/world_export_synthetic.h
                                  skinned_probe()): bar centres at the clip's first and
                                  last frame, winding and mirroring; prints
                                  SKINNED_PROBE ... negScale=<0|1> OK or ... FAIL <why>

Spec: docs/superpowers/specs/2026-10-15-world-export-design.md D6 ("Blender",
"Frame probe"). The expected directions are exportgltf::frame_probe_expect()
(include/castlemist/exportgltf/world_probe.h), from world/frame.h's
map_to_blender: east +X, north +Y, up +Z, bar lengths 3 : 2 : 1.

Frame of a .glb. Every world .glb has the root GW2_MapToGltf = map_to_gltf_root(1),
a reflection; imported raw, every object under it has det < 0. bake_objects()
removes it before anything is measured, instanced or exported:
  * meshes get their world transform applied into the vertex data, and the
    mirrored ones their winding flipped back (Mesh.flip_normals);
  * an armature is rebuilt at identity in the same un-mirrored frame: with W its
    world matrix (W = s Q, Q orthogonal, det W < 0) and F = diag(1, 1, -1) in bone
    space, every rest bone R becomes Q R F with its head at W head, and every pose
    / keyframe basis P becomes F P F (location z, quaternion x and y, euler x and
    y negate; locations scale by s). Then each bone's deform is W D W^-1, so the
    skinned mesh, baked to W v, deforms exactly as before -- with no det < 0 left.
    Measured by --check-skinned-probe on the .glb and on its .fbx.

FBX axis settings (Task 1 frame probe, measured 2026-10-15 with Blender 4.5 and
Unity 6000.0.42f1; the same on 2022.3.22f1, Task 12): the VRChat script's model mode, axis_forward='-Z',
axis_up='Y'. Unity honours the axis system the FBX declares, so axis_forward
'-Z' and 'Z' import to the same world result there. What decides Unity's
result is ModelImporter.bakeAxisConversion: off, east and north come out as
-X and -Z (a 180 degree turn about Y); on, they come out as +X and +Z, as
map_to_unity says. The importer sets it on (FrameProbe.BakeAxisConversion).
Armature models go through the same settings (their armature is at identity
after the bake, so bake_space_transform stays off for every file).

Textures in the FBX (ruling W8): not embedded and not copied. path_mode='RELATIVE'
writes each image's path relative to the .fbx, so a model's FBX points at the
shared pool (../../textures/<fileId>.png) and a cell's at its own PNGs, the files
already in the folder. Unity's importer builds its materials from materials.json
(spec D6) and only needs the FBX for geometry, rig and clips.
"""
import json
import math
import os
import re
import struct
import sys
import time

import bpy
from mathutils import Matrix, Quaternion, Vector
from mathutils.kdtree import KDTree

# Blender's -Y (its "forward") -> FBX -Z: castlemist_vrchat.py's setting.
FBX_AXIS_FORWARD = '-Z'
FBX_AXIS_UP = 'Y'

PROBE_EXPECT = {"east": (1.0, 0.0, 0.0), "north": (0.0, 1.0, 0.0), "up": (0.0, 0.0, 1.0)}
PROBE_BARS = {"east": "Probe_East", "north": "Probe_North", "up": "Probe_Up"}
PROBE_LENGTHS = (3.0, 2.0, 1.0)
PROBE_ROOTS = ("GW2_MapToGltf", "probe")

# map -> Blender (world/frame.h map_to_blender): (x, y, z) -> (x, y, -z).
C_B = Matrix(((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, -1, 0), (0, 0, 0, 1)))
# Unity (map_to_unity: (x, -z, y)) -> Blender: swap y and z.
P_UB = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))

# The skinned probe's bar centres (Blender frame) at the clip's first and last frame.
SKINNED_EXPECT = {
    "East": ((150.0, 0.0, 0.0), (250.0, 0.0, 0.0)),
    "North": ((0.0, 100.0, 0.0), (-100.0, 0.0, 0.0)),
    "Up": ((0.0, 0.0, 50.0), (0.0, 0.0, 100.0)),
}
SKINNED_TOLERANCE = 0.5

_SUFFIX = re.compile(r"\.\d{3,}$")

# castlemist-foliage-1 (world-foliage spec D7): 100 bytes, little-endian.
FOLIAGE_RECORD = struct.Struct("<IHHI12f3f4f3f")
FOLIAGE_GROUP = "CM_Foliage"


def base_name(name):
    """A name without Blender's ".001" de-duplication suffix."""
    return _SUFFIX.sub("", name)


def empty_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


# glTF (x, y, z) -> Blender (x, -z, y): the glTF importer's axis conversion.
GLTF_TO_BLENDER = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
TMP_SUFFIX = ".castlemist_tmp.glb"


def _read_glb(path):
    import struct
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 20 or data[:4] != b"glTF":
        return None
    n, kind = struct.unpack_from("<II", data, 12)
    if kind != 0x4E4F534A:  # "JSON"
        return None
    return json.loads(data[20:20 + n].decode("utf-8")), data[20 + n:]


def _write_glb(path, gltf, rest):
    import struct
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * (-len(js) % 4)
    total = 12 + 8 + len(js) + len(rest)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(rest)


def import_gltf(path):
    """bpy.ops.import_scene.gltf, except for a skinned .glb under a mirrored root
    (GW2_MapToGltf = map_to_gltf_root(1), det -1): Blender 4.5's glTF importer
    decomposes the mirrored node matrices of such a rig wrongly (measured with
    the skinned probe: the raw import plays the clip mirrored, East moving -100
    instead of +100 and North turning toward +X instead of -X). So the root's
    matrix is taken out of a temporary copy beside the file (relative image URIs
    still resolve), the copy is imported, and the root object then gets the
    root matrix in Blender's frame, GLTF_TO_BLENDER R GLTF_TO_BLENDER^-1, as an
    object transform -- which Blender evaluates exactly; bake_objects() then
    takes it out of the armature (module docstring)."""
    parsed = _read_glb(path) if path.lower().endswith(".glb") else None
    root = None
    if parsed is not None and parsed[0].get("skins"):
        gltf = parsed[0]
        scenes = gltf.get("scenes") or []
        top = (scenes[gltf.get("scene", 0)].get("nodes") or []) if scenes else []
        for ni in top:
            node = gltf["nodes"][ni]
            m = node.get("matrix")
            if m and len(m) == 16:
                R = Matrix([[float(m[c * 4 + r]) for c in range(4)] for r in range(4)])
                if R.determinant() < 0:
                    root = (node.get("name"), R)
                    del node["matrix"]
                    break
    if root is None:
        bpy.ops.import_scene.gltf(filepath=path)
        return
    tmp = path[:-4] + TMP_SUFFIX
    before = set(o.name for o in bpy.data.objects)
    try:
        _write_glb(tmp, parsed[0], parsed[1])
        bpy.ops.import_scene.gltf(filepath=tmp)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    new = [o for o in bpy.data.objects if o.name not in before]
    name, R = root
    ob = next((o for o in new if o.parent is None and base_name(o.name) == name), None)
    if ob is None:
        ob = next((o for o in new if o.parent is None), None)
    if ob is not None:
        ob.matrix_basis = GLTF_TO_BLENDER @ R @ GLTF_TO_BLENDER.inverted() @ ob.matrix_basis
    bpy.context.view_layer.update()


def import_any(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        import_gltf(path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    else:
        raise SystemExit("cannot import " + path)


def curves_of(action):
    # Blender 4.4+ slotted actions keep curves in layers/strips/channelbags;
    # action.fcurves still works through 4.5 for single-slot actions.
    try:
        fcs = list(action.fcurves)
    except AttributeError:
        fcs = []
    if fcs:
        return fcs
    for layer in getattr(action, "layers", []):
        for strip in layer.strips:
            for bag in getattr(strip, "channelbags", []):
                fcs.extend(bag.fcurves)
    return fcs


def assign_action(ob, action):
    ad = ob.animation_data or ob.animation_data_create()
    ad.action = action
    # Slotted actions (4.4+): a copied object's name matches no slot, so pick one.
    if hasattr(ad, "action_slot") and action is not None and ad.action_slot is None and len(action.slots):
        ad.action_slot = action.slots[0]


# ------------------------------------------------------------------ probe check --

def world_bounds(ob):
    pts = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    return lo, hi


def find_bar(objs, name):
    # FBX / glTF importers may suffix names (".001") or name the mesh, not the object.
    for o in objs:
        if o.type == 'MESH' and (o.name == name or o.name.startswith(name + ".") or o.data.name == name):
            return o
    return None


def fmt(v):
    return "(%.3f,%.3f,%.3f)" % tuple(round(c, 3) + 0.0 for c in v)  # no "-0.000"


def neg_scale(objs):
    return 1 if any(o.matrix_world.determinant() < 0 for o in objs) else 0


def inward_faces(ob, centre, me=None, matrix=None, centre_of=None):
    """Faces whose world-space winding normal, or any of whose corner (shading)
    normals, point into the box: a reflection applied without flipping the
    winding shows up here. `centre_of(polygon)` overrides `centre` per face."""
    m = matrix if matrix is not None else ob.matrix_world
    n3 = m.to_3x3().inverted_safe().transposed()
    # Under det < 0 a face's winding normal is the negated n3 image.
    flip = -1.0 if m.determinant() < 0 else 1.0
    me = me if me is not None else ob.data
    bad = 0
    for p in me.polygons:
        c = centre_of(p) if centre_of else centre
        out = m @ p.center - c
        if flip * (n3 @ p.normal).dot(out) <= 0:
            bad += 1
        # The shading (custom split) normals the FBX carries to Unity.
        for li in p.loop_indices:
            if (n3 @ me.corner_normals[li].vector).dot(out) <= 0:
                bad += 1
    return bad


def measure_probe(objs, origin):
    """world_probe.h's check rule on the probe's (baked) objects, measured from
    `origin`. Returns (ok, line, extents line or None)."""
    neg = neg_scale(objs)
    dirs, extents, fails = {}, {}, []
    for axis, name in PROBE_BARS.items():
        ob = find_bar(objs, name)
        if ob is None:
            dirs[axis] = Vector((0.0, 0.0, 0.0))
            fails.append(axis)
            continue
        lo, hi = world_bounds(ob)
        c = (lo + hi) * 0.5 - origin
        d = c.normalized() if c.length > 0 else c
        dirs[axis] = d
        want = Vector(PROBE_EXPECT[axis])
        size = hi - lo
        extents[axis] = abs(size.dot(want))
        if d.dot(want) <= 0.99:
            fails.append(axis)
        if inward_faces(ob, (lo + hi) * 0.5):
            fails.append(axis + "-winding")
    if not fails:
        unit = extents["up"]
        for axis, ratio in (("east", PROBE_LENGTHS[0]), ("north", PROBE_LENGTHS[1])):
            if unit <= 0 or abs(extents[axis] / unit - ratio) > 0.01 * ratio:
                fails.append(axis + "-length")
    line = "PROBE east=%s north=%s up=%s negScale=%d" % (fmt(dirs["east"]), fmt(dirs["north"]), fmt(dirs["up"]), neg)
    line += " OK" if not fails and neg == 0 else " FAIL " + ",".join(fails or ["negScale"])
    ext = ("PROBE_EXTENTS " + " ".join("%s=%.4f" % (k, v) for k, v in extents.items())) if extents else None
    return not fails and neg == 0, line, ext


def check_probe(path):
    """Imports the probe as the pipeline does and measures it (world_probe.h's
    check rule). A .glb is imported raw -- the reflected root leaves every
    object at det < 0, printed as PROBE_RAW -- and then gets bake_objects(),
    the same step --to-fbx and the importer apply, before it is measured."""
    empty_scene()
    import_any(path)
    bpy.context.view_layer.update()
    objs = list(bpy.context.scene.objects)
    root = next((o for o in objs if o.name in PROBE_ROOTS), None)
    origin = root.matrix_world.translation.copy() if root else Vector((0.0, 0.0, 0.0))
    if os.path.splitext(path)[1].lower() in (".glb", ".gltf"):
        print("PROBE_RAW negScale=%d objects=%d" % (neg_scale(objs), len(objs)))
        bake_objects(objs)
        bpy.context.view_layer.update()
        objs = list(bpy.context.scene.objects)
    ok, line, ext = measure_probe(objs, origin)
    print(line)
    if ext:
        print(ext)
    return ok


# ------------------------------------------------------------ skinned probe check --

def group_centres(ob, dg):
    """World-space centre of each vertex group's vertices on the evaluated
    (deformed) mesh; the evaluated mesh and its world matrix."""
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    m = ev.matrix_world.copy()
    names = {g.index: g.name for g in ob.vertex_groups}
    sums, counts, vgroup = {}, {}, {}
    for v in ob.data.vertices:
        if not v.groups:
            continue
        g = max(v.groups, key=lambda e: e.weight)
        n = names.get(g.group)
        vgroup[v.index] = n
        sums[n] = sums.get(n, Vector()) + m @ me.vertices[v.index].co
        counts[n] = counts.get(n, 0) + 1
    centres = {n: sums[n] / counts[n] for n in sums}
    return centres, me, m, vgroup, ev


def check_skinned_probe(path):
    """The skinned probe through the same steps as a model: a .glb gets
    bake_objects() (printed raw first as SKINNED_RAW); an .fbx is measured as
    Blender's FBX importer gives it. At the clip's first and last frame each
    bar's deformed centre must match SKINNED_EXPECT, every face and corner
    normal must point out of its bar, and no object may have det < 0."""
    empty_scene()
    import_any(path)
    bpy.context.view_layer.update()
    scene = bpy.context.scene
    is_glb = os.path.splitext(path)[1].lower() in (".glb", ".gltf")

    def measure(tag):
        objs = list(scene.objects)
        arm = next((o for o in objs if o.type == 'ARMATURE'), None)
        mesh = next((o for o in objs if o.type == 'MESH' and len(o.vertex_groups)), None)
        if arm is None or mesh is None:
            print("%s FAIL no armature or skinned mesh (objects: %s)" % (tag, ", ".join(o.name for o in objs)))
            return False
        act = arm.animation_data.action if arm.animation_data else None
        if act is None or "ProbeClip" not in act.name:
            act = next((a for a in bpy.data.actions if "ProbeClip" in a.name), None)
            if act is not None:
                assign_action(arm, act)
        if act is None:
            print("%s FAIL no ProbeClip action" % tag)
            return False
        f0, f1 = (int(round(f)) for f in act.frame_range)
        fails, parts = [], []
        for which, frame in ((0, f0), (1, f1)):
            scene.frame_set(frame)
            dg = bpy.context.evaluated_depsgraph_get()
            centres, me, m, vgroup, ev = group_centres(mesh, dg)
            for bar, want in SKINNED_EXPECT.items():
                got = centres.get(bar)
                parts.append("%s=%s" % (bar, fmt(got) if got is not None else "none"))
                if got is None or (got - Vector(want[which])).length > SKINNED_TOLERANCE:
                    fails.append("%s@%s" % (bar, "end" if which else "start"))
            loops = me.loops
            bad = inward_faces(mesh, None, me=me, matrix=m,
                               centre_of=lambda p: centres.get(vgroup.get(loops[p.loop_start].vertex_index), Vector()))
            if bad:
                fails.append("winding@%s=%d" % ("end" if which else "start", bad))
            ev.to_mesh_clear()
            if which == 0:
                parts.append("|")
        neg = neg_scale(objs)
        line = "%s frames=%d..%d %s negScale=%d" % (tag, f0, f1, " ".join(parts), neg)
        print(line + (" OK" if not fails else " FAIL " + ",".join(fails)))
        return not fails and neg == 0

    if is_glb:
        measure("SKINNED_RAW")
        bake_objects(list(scene.objects))
        bpy.context.view_layer.update()
    return measure("SKINNED_PROBE")


# ------------------------------------------------------------------- the bake --

def apply_transforms(objs):
    # The shared-data copy step of castlemist_vrchat.py: transform_apply refuses
    # shared data, and the glTF importer shares one mesh between nodes that
    # instance the same glTF mesh. Applying a reflection also flips the winding.
    for o in objs:
        if o.type == 'MESH' and o.data.users > 1:
            o.data = o.data.copy()
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def unparent_keep(o):
    world = o.matrix_world.copy()
    o.parent = None
    o.matrix_world = world


def bake_meshes(meshes):
    """Every transform (the reflected GW2_MapToGltf root included) into the
    vertex data; no mesh is left with det < 0."""
    if not meshes:
        return
    mirrored = [o for o in meshes if o.matrix_world.determinant() < 0]
    for o in meshes:
        if o.parent is not None:
            unparent_keep(o)
    apply_transforms(meshes)
    # Blender 4.5's transform_apply moves the vertices through a reflection
    # but keeps the winding (measured by the Task 1 probe: every face and
    # corner normal ends up pointing inward). Flip those meshes back;
    # Mesh.flip_normals also flips custom split normals.
    for o in mirrored:
        o.data.flip_normals()


_POSE_PATH = re.compile(r'^pose\.bones\["(.+)"\]\.(location|rotation_quaternion|rotation_euler|rotation_axis_angle)$')


def basis_factor(prop, idx, mirrored, s):
    """The factor F P F (F = diag(1, 1, -1) when mirrored) and the scale s put on
    one channel of a pose basis; see the module docstring."""
    neg = False
    if mirrored:
        neg = (prop == "location" and idx == 2) or (prop == "rotation_quaternion" and idx in (1, 2)) or \
              (prop == "rotation_euler" and idx in (0, 1)) or (prop == "rotation_axis_angle" and idx in (1, 2))
    f = -1.0 if neg else 1.0
    return f * s if prop == "location" else f


def bake_armature(arm, actions, converted):
    """Rebuilds `arm` at identity in its un-mirrored world frame (module
    docstring). `actions`: the actions that may drive it; each is converted once
    (`converted` holds the names already done). Its meshes must already be
    unparented and baked."""
    W = arm.matrix_world.copy()
    W3 = W.to_3x3()
    det = W3.determinant()
    s = abs(det) ** (1.0 / 3.0) if det != 0 else 1.0
    Q = W3 * (1.0 / s)
    mirrored = det < 0
    F = Matrix.Diagonal((1.0, 1.0, -1.0)) if mirrored else Matrix.Identity(3)
    arm.parent = None
    arm.matrix_parent_inverse = Matrix.Identity(4)
    arm.matrix_world = Matrix.Identity(4)
    bpy.context.view_layer.update()

    bpy.ops.object.select_all(action='DESELECT')
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = arm.data.edit_bones
    target = {}
    for eb in ebs:
        R = eb.matrix.to_3x3().normalized()
        target[eb.name] = (Q @ R @ F, W @ eb.head, eb.length * s)

    def depth(eb):
        d = 0
        while eb.parent is not None:
            eb, d = eb.parent, d + 1
        return d

    # Parents first: a connected child's head follows its parent's tail.
    for eb in sorted(ebs, key=depth):
        rot, head, length = target[eb.name]
        m = rot.to_4x4()
        m.translation = head
        eb.matrix = m
        eb.length = length
    bpy.ops.object.mode_set(mode='OBJECT')

    bones = set(b.name for b in arm.data.bones)
    for act in actions:
        if act is None or act.name in converted:
            continue
        curves = [fc for fc in curves_of(act) if _POSE_PATH.match(fc.data_path)]
        if not any(_POSE_PATH.match(fc.data_path).group(1) in bones for fc in curves):
            continue
        converted.add(act.name)
        for fc in curves:
            mt = _POSE_PATH.match(fc.data_path)
            f = basis_factor(mt.group(2), fc.array_index, mirrored, s)
            if f == 1.0:
                continue
            for kp in fc.keyframe_points:
                kp.co.y *= f
                kp.handle_left.y *= f
                kp.handle_right.y *= f
            fc.update()
    for pb in arm.pose.bones:
        for prop in ("location", "rotation_quaternion", "rotation_euler", "rotation_axis_angle"):
            v = list(getattr(pb, prop))
            setattr(pb, prop, [c * basis_factor(prop, i, mirrored, s) for i, c in enumerate(v)])


def owning_armature(o):
    p = o.parent
    while p is not None:
        if p.type == 'ARMATURE':
            return p
        p = p.parent
    for m in o.modifiers:
        if m.type == 'ARMATURE' and m.object is not None:
            return m.object
    return None


def bake_objects(objs, actions=None):
    """After a .glb import of `objs`: no object is left with det < 0 (module
    docstring). Meshes are baked; armatures rebuilt at identity, with their
    meshes parented back as before (to the armature, or to the same bone);
    empties and the glTF importer's bone shapes are removed. `actions`: the
    actions the import created (default: all). Returns the armatures."""
    names = [o.name for o in objs if o.name in bpy.data.objects]

    def alive():
        return [bpy.data.objects[n] for n in names if n in bpy.data.objects]

    objs = alive()
    # The glTF importer's bone display shapes (collection glTF_not_exported).
    shapes = set()
    for o in objs:
        if o.type == 'ARMATURE':
            for pb in o.pose.bones:
                if pb.custom_shape is not None:
                    shapes.add(pb.custom_shape.name)
                    pb.custom_shape = None
    for n in shapes:
        bpy.data.objects.remove(bpy.data.objects[n])
    junk = bpy.data.collections.get("glTF_not_exported")
    if junk is not None and not junk.objects:
        bpy.data.collections.remove(junk)
    names = [n for n in names if n not in shapes]
    objs = alive()

    arms = [o for o in objs if o.type == 'ARMATURE']
    meshes = [o for o in objs if o.type == 'MESH']
    for o in objs:
        if o.type != 'ARMATURE' and o.animation_data and o.animation_data.action:
            print("BAKE_WARN object animation on %s is not converted" % o.name)
    links = {}
    for o in meshes:
        a = owning_armature(o)
        if a is not None:
            bone = o.parent_bone if (o.parent == a and o.parent_type == 'BONE') else ""
            links[o.name] = (a, bone)
    bake_meshes(meshes)
    if actions is None:
        actions = list(bpy.data.actions)
    converted = set()
    for a in arms:
        bake_armature(a, actions, converted)
    bpy.context.view_layer.update()
    for o in meshes:
        if o.name not in links:
            continue
        a, bone = links[o.name]
        if bone:
            # Bone parenting is relative to the bone's rest tail: place it at rest.
            a.data.pose_position = 'REST'
            bpy.context.view_layer.update()
            o.parent = a
            o.parent_type = 'BONE'
            o.parent_bone = bone
            o.matrix_parent_inverse = Matrix.Identity(4)
            bpy.context.view_layer.update()
            o.matrix_world = Matrix.Identity(4)
            a.data.pose_position = 'POSE'
        else:
            o.parent = a
            o.parent_type = 'OBJECT'
            o.matrix_parent_inverse = Matrix.Identity(4)
            o.matrix_world = Matrix.Identity(4)
    for o in [o for o in alive() if o.type == 'EMPTY']:
        for c in list(o.children):
            unparent_keep(c)
        bpy.data.objects.remove(o)
    bpy.context.view_layer.update()
    return arms


# ------------------------------------------------------------------ glb -> fbx --

FBX_TMP_SUFFIX = ".castlemist_tmp.fbx"


def glb_to_fbx(src, out):
    """Writes <out> through a temporary file and a rename, so a killed or failed
    Blender never leaves a truncated .fbx newer than its .glb (review M3)."""
    tmp = out[:-4] + FBX_TMP_SUFFIX
    try:
        _glb_to_fbx(src, tmp)
        os.replace(tmp, out)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def _glb_to_fbx(src, out):
    empty_scene()
    import_gltf(src)
    bake_objects(list(bpy.context.scene.objects))
    bpy.ops.export_scene.fbx(
        filepath=out,
        use_selection=False,
        object_types={'ARMATURE', 'MESH'},
        apply_scale_options='FBX_SCALE_ALL',
        apply_unit_scale=True,
        axis_forward=FBX_AXIS_FORWARD,
        axis_up=FBX_AXIS_UP,
        bake_space_transform=False,   # armatures are at identity after the bake
        add_leaf_bones=False,
        primary_bone_axis='Y',
        secondary_bone_axis='X',
        use_armature_deform_only=False,
        use_mesh_modifiers=False,
        mesh_smooth_type='FACE',
        bake_anim=True,
        bake_anim_use_all_actions=True,
        bake_anim_use_nla_strips=False,
        bake_anim_force_startend_keying=True,
        bake_anim_simplify_factor=0.0,
        path_mode='RELATIVE',   # W8: textures stay where they are, by relative path
        embed_textures=False,
    )


def to_fbx(export_dir):
    t0 = time.time()
    done = skipped = failed = 0
    for dirpath, _dirs, files in os.walk(export_dir):
        for f in sorted(files):
            if not f.lower().endswith(".glb") or f.endswith(TMP_SUFFIX):
                continue
            src = os.path.join(dirpath, f)
            out = os.path.splitext(src)[0] + ".fbx"
            if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
                skipped += 1
                continue
            try:
                glb_to_fbx(src, out)
                done += 1
            except Exception as e:  # one bad file never stops the session
                failed += 1
                print("TO_FBX_FAIL %s: %s: %s" % (src, type(e).__name__, e))
    print("TO_FBX converted=%d skipped=%d failed=%d seconds=%.1f" % (done, skipped, failed, time.time() - t0))
    n = set_manifest_fbx(export_dir)
    if n is not None:
        print("TO_FBX_MANIFEST fbxSet=%d" % n)
    return failed == 0


def set_manifest_fbx(export_dir):
    """The manifest's fbx fields, as exportgltf::set_manifest_fbx (world_manifest.cpp)
    sets them: on every cell, model and the frameProbe with a glb, the .fbx beside
    it when that file exists, else null. Written to manifest.json.tmp, then
    renamed. Returns the number set, or None when there is no castlemist world
    manifest (review I3: --to-fbx alone is enough for the Unity importer)."""
    path = os.path.join(export_dir, "manifest.json")
    try:
        with open(path, "rb") as f:
            man = json.loads(f.read().decode("utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(man, dict) or man.get("format") != "castlemist-world":
        return None
    n = 0

    def fill(block):
        nonlocal n
        if not isinstance(block, dict) or not isinstance(block.get("glb"), str):
            return
        rel = os.path.splitext(block["glb"])[0] + ".fbx"
        if os.path.isfile(os.path.join(export_dir, rel)):
            block["fbx"] = rel.replace("\\", "/")
            n += 1
        else:
            block["fbx"] = None

    for key in ("cells", "models"):
        for b in man.get(key) or []:
            fill(b)
    fill(man.get("frameProbe"))
    tmp = path + ".tmp"
    try:
        with open(tmp, "wb") as f:
            f.write((json.dumps(man, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        os.replace(tmp, path)
    except OSError as e:
        print("TO_FBX_MANIFEST_FAIL %s: %s" % (path, e))
        if os.path.exists(tmp):
            os.remove(tmp)
        return None
    return n


# -------------------------------------------------------------------- import --

def new_collection(name, parent):
    c = bpy.data.collections.new(name)
    parent.children.link(c)
    return c


def layer_collection(col, lc=None):
    lc = lc or bpy.context.view_layer.layer_collection
    if lc.collection == col:
        return lc
    for ch in lc.children:
        r = layer_collection(col, ch)
        if r is not None:
            return r
    return None


def move_to(o, col):
    for c in list(o.users_collection):
        c.objects.unlink(o)
    col.objects.link(o)


def alive(names):
    return [bpy.data.objects[n] for n in names if n in bpy.data.objects]


def import_glb(path):
    """Imports into the scene's own collection; returns (object names, actions) it created."""
    before_o = set(o.name for o in bpy.data.objects)
    before_a = set(a.name for a in bpy.data.actions)
    bpy.context.view_layer.active_layer_collection = bpy.context.view_layer.layer_collection
    import_gltf(path)
    names = [o.name for o in bpy.data.objects if o.name not in before_o]
    acts = [a for a in bpy.data.actions if a.name not in before_a]
    return names, acts


def image_nodes_of(mat):
    if mat is None or not mat.use_nodes or mat.node_tree is None:
        return []
    return [n for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image is not None]


def image_ok(img):
    path = bpy.path.abspath(img.filepath) if img.filepath else ""
    return (img.packed_file is not None or os.path.isfile(path)) and img.size[0] > 0 and img.size[1] > 0


def materials_of(objs):
    mats = []
    for o in objs:
        if o.type == 'MESH':
            for m in o.data.materials:
                if m is not None and m not in mats:
                    mats.append(m)
    return mats


def base_color_images(mat):
    """Image nodes upstream of the Principled BSDF's Base Color (else every image node)."""
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is None:
        return image_nodes_of(mat)
    found, seen, stack = [], set(), [bsdf.inputs["Base Color"]]
    while stack:
        sock = stack.pop()
        for link in sock.links:
            n = link.from_node
            if n.name in seen:
                continue
            seen.add(n.name)
            if n.type == 'TEX_IMAGE':
                found.append(n)
            stack.extend(i for i in n.inputs if i.is_linked)
    return found or image_nodes_of(mat)


def add_uv_scroll(mat, uv_index, scroll, uv_name):
    """A Mapping node before each base-colour image, its Location driven by
    frame / fps * scroll. glTF V is flipped in Blender's UVs, so V scrolls
    with the opposite sign. Returns the count of nodes driven."""
    nt = mat.node_tree
    n_done = 0
    for img in base_color_images(mat):
        vec = img.inputs["Vector"]
        src = vec.links[0].from_socket if vec.is_linked else None
        if src is not None and src.node.type == 'MAPPING':
            mapping = src.node
        else:
            mapping = nt.nodes.new('ShaderNodeMapping')
            mapping.vector_type = 'POINT'
            mapping.location = (img.location.x - 200, img.location.y)
            if src is None:
                uv = nt.nodes.new('ShaderNodeUVMap')
                uv.uv_map = uv_name
                uv.location = (mapping.location.x - 200, mapping.location.y)
                src = uv.outputs["UV"]
            nt.links.new(src, mapping.inputs["Vector"])
            nt.links.new(mapping.outputs["Vector"], vec)
        loc = mapping.inputs["Location"]
        for axis, rate in ((0, float(scroll[0])), (1, -float(scroll[1]))):
            fc = loc.driver_add("default_value", axis)
            drv = fc.driver
            drv.type = 'SCRIPTED'
            var = drv.variables.new()
            var.name = "fps"
            var.type = 'SINGLE_PROP'
            var.targets[0].id_type = 'SCENE'
            var.targets[0].id = bpy.context.scene
            var.targets[0].data_path = "render.fps"
            drv.expression = "frame / fps * (%r)" % rate
        mapping["castlemist_uv_scroll"] = [float(scroll[0]), float(scroll[1])]
        mapping["castlemist_uv_index"] = int(uv_index)
        n_done += 1
    return n_done


def make_dark_decal(mat):
    # EEVEE Legacy's blend_method; 4.2+ maps it onto surface_render_method.
    try:
        mat.blend_method = 'BLEND'
    except (AttributeError, TypeError):
        pass
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = 'BLENDED'


def loop_action(act):
    for fc in curves_of(act):
        if not any(m.type == 'CYCLES' for m in fc.modifiers):
            fc.modifiers.new('CYCLES')


class ModelEntry:
    def __init__(self, index, file_id, col, actions):
        self.index, self.file_id, self.col, self.actions = index, file_id, col, actions
        self.clip_cols = {}


def clip_collection(entry, clip, models_col, warnings):
    """`<fileId>_<clip>`: a copy of the model's objects (data shared) whose
    armature plays that clip, looping."""
    if clip in entry.clip_cols:
        return entry.clip_cols[clip]
    act = entry.actions.get(clip)
    arms = [o for o in entry.col.objects if o.type == 'ARMATURE']
    if act is None or not arms:
        warnings.append("model %d: clip %s not found; its instances are static" % (entry.file_id, clip))
        entry.clip_cols[clip] = entry.col
        return entry.col
    col = new_collection("%d_%s" % (entry.file_id, clip), models_col)
    copies = {}
    for o in entry.col.objects:
        c = o.copy()
        col.objects.link(c)
        copies[o.name] = c
    for name, c in copies.items():
        o = bpy.data.objects[name]
        if o.parent is not None and o.parent.name in copies:
            c.parent = copies[o.parent.name]
            c.matrix_parent_inverse = o.matrix_parent_inverse.copy()
        for m in c.modifiers:
            if m.type == 'ARMATURE' and m.object is not None and m.object.name in copies:
                m.object = copies[m.object.name]
        if c.type == 'ARMATURE':
            assign_action(c, act)
    loop_action(act)
    entry.clip_cols[clip] = col
    return col


def matrix_of(m16):
    """A manifest column-major 4x4 as a mathutils Matrix."""
    return Matrix([[float(m16[c * 4 + r]) for c in range(4)] for r in range(4)])


def read_foliage(path):
    """The records of one foliage/<cell>.bin: dicts with model, layer, flags,
    placement, m (column-major 4x4, map space, absolute) and unity (t, r, s).
    Raises ValueError when the size is not a whole number of records."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % FOLIAGE_RECORD.size:
        raise ValueError("%d bytes is not a multiple of %d" % (len(data), FOLIAGE_RECORD.size))
    out = []
    for at in range(0, len(data), FOLIAGE_RECORD.size):
        v = FOLIAGE_RECORD.unpack_from(data, at)
        a = v[4:16]
        out.append({"model": v[0], "layer": v[1], "flags": v[2], "placement": v[3],
                    "m": [a[0], a[1], a[2], 0.0, a[3], a[4], a[5], 0.0, a[6], a[7], a[8], 0.0,
                          a[9], a[10], a[11], 1.0],
                    "unity": {"t": list(v[16:19]), "r": list(v[19:23]), "s": v[23]}})
    return out


def euler_xyz(r):
    """Blender's XYZ Euler (R = Rz Ry Rx) of a 3x3 rotation given as rows, in
    double precision (mathutils works in float, which loses about 1e-2 near a
    90 degree Y turn; review I2)."""
    cy = math.hypot(r[0][0], r[1][0])
    if cy > 1e-9:
        return (math.atan2(r[2][1], r[2][2]), math.atan2(-r[2][0], cy), math.atan2(r[1][0], r[0][0]))
    return (math.atan2(-r[1][2], r[1][1]), math.atan2(-r[2][0], cy), 0.0)


def foliage_trs(m16, S):
    """(location, cm_rot, cm_scale) of S C_b M C_b^-1 (C_b^-1 = C_b), computed
    from the record's m in doubles: the instance's Blender matrix."""
    a = [[float(m16[c * 4 + r]) for c in range(3)] for r in range(3)]
    f = (1.0, 1.0, -1.0)
    b = [[S * f[i] * a[i][j] * f[j] for j in range(3)] for i in range(3)]
    sc = [math.sqrt(sum(b[i][j] ** 2 for i in range(3))) for j in range(3)]
    rot = [[b[i][j] / sc[j] if sc[j] > 0 else 0.0 for j in range(3)] for i in range(3)]
    loc = (S * float(m16[12]), S * float(m16[13]), -S * float(m16[14]))
    return loc, euler_xyz(rot), sc


def foliage_node_group():
    """CM_Foliage: Instance on Points of the Collection input, rotation from the
    cm_rot point attribute (Euler XYZ), scale from cm_scale; not realised."""
    ng = bpy.data.node_groups.get(FOLIAGE_GROUP)
    if ng is not None:
        return ng
    ng = bpy.data.node_groups.new(FOLIAGE_GROUP, 'GeometryNodeTree')
    ng.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    ng.interface.new_socket("Collection", in_out='INPUT', socket_type='NodeSocketCollection')
    ng.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    n, ln = ng.nodes, ng.links
    gi = n.new('NodeGroupInput')
    go = n.new('NodeGroupOutput')
    ci = n.new('GeometryNodeCollectionInfo')
    ci.transform_space = 'ORIGINAL'
    iop = n.new('GeometryNodeInstanceOnPoints')
    rot = n.new('GeometryNodeInputNamedAttribute')
    rot.data_type = 'FLOAT_VECTOR'
    rot.inputs["Name"].default_value = "cm_rot"
    e2r = n.new('FunctionNodeEulerToRotation')
    scl = n.new('GeometryNodeInputNamedAttribute')
    scl.data_type = 'FLOAT_VECTOR'
    scl.inputs["Name"].default_value = "cm_scale"
    ln.new(gi.outputs["Geometry"], iop.inputs["Points"])
    ln.new(gi.outputs["Collection"], ci.inputs["Collection"])
    ln.new(ci.outputs["Instances"], iop.inputs["Instance"])
    ln.new(rot.outputs["Attribute"], e2r.inputs["Euler"])
    ln.new(e2r.outputs["Rotation"], iop.inputs["Rotation"])
    ln.new(scl.outputs["Attribute"], iop.inputs["Scale"])
    ln.new(iop.outputs["Instances"], go.inputs["Geometry"])
    for k, nd in enumerate((gi, ci, rot, e2r, scl, iop, go)):
        nd.location = (k * 200.0, 0.0)
    return ng


def foliage_collection_key():
    return foliage_node_group().interface.items_tree["Collection"].identifier


def foliage_object(name, col, records, S, collection):
    """One object at identity: a vertex per record at its Blender position, the
    cm_rot and cm_scale point attributes, and the CM_Foliage modifier on the
    model's collection."""
    trs = [foliage_trs(r["m"], S) for r in records]
    me = bpy.data.meshes.new(name)
    me.from_pydata([t[0] for t in trs], [], [])
    a = me.attributes.new("cm_rot", 'FLOAT_VECTOR', 'POINT')
    a.data.foreach_set("vector", [v for t in trs for v in t[1]])
    a = me.attributes.new("cm_scale", 'FLOAT_VECTOR', 'POINT')
    a.data.foreach_set("vector", [v for t in trs for v in t[2]])
    ob = bpy.data.objects.new(name, me)
    col.objects.link(ob)
    mod = ob.modifiers.new(FOLIAGE_GROUP, 'NODES')
    mod.node_group = foliage_node_group()
    mod[foliage_collection_key()] = collection
    return ob


def import_world(export_dir, colliders=True, dark_decal=True, scale=1.0, verify=False):
    """Builds the scene from <export_dir>/manifest.json (spec D6 "Blender"):
      GW2 <fileId>
        Models (excluded from the view layer): <fileId> per model from its .glb,
               baked (bake_objects), and <fileId>_<clip> per clip placed
        Cells/<id>: <id> Terrain, <id> Water, <id> Props (collection-instance
               empties, matrix_world = S C_b M C_b^-1, C_b = diag(1, 1, -1),
               S = scale), <id> Colliders (hidden) and <id> Foliage: one
               geometry-nodes object "<id> <fileId> foliage" per model with
               records in the cell's foliage bin (world-foliage D8), a point
               per record, instanced by the shared CM_Foliage group
        A missing or malformed foliage bin is an IMPORT_ERROR naming it (the
        import returns False); records of a failed or missing model are skipped
        and counted.
        Frame Probe (probe.glb at frameProbe.map), Spawn, Sun
      World: sky.equirect as an Environment Texture, else the ambient colour.
    Dark-decal materials get blend_method 'BLEND' (unless dark_decal is off);
    uvScroll entries get a Mapping-node driver frame / fps * scroll. Returns
    True, or with verify the verification's result."""
    t0 = time.time()
    with open(os.path.join(export_dir, "manifest.json"), "rb") as f:
        man = json.loads(f.read().decode("utf-8"))
    if bpy.app.background:
        empty_scene()
    scene = bpy.context.scene
    S = Matrix.Scale(float(scale), 4)
    warnings = []
    file_id = (man.get("map") or {}).get("fileId", 0)
    root = new_collection("GW2 %s" % file_id, scene.collection)
    models_col = new_collection("Models", root)
    cells_col = new_collection("Cells", root)

    # Models, baked once each.
    entries = {}
    n_decal = n_scroll = 0
    for k, m in enumerate(man.get("models") or []):
        if m.get("status") != "ok" or not m.get("glb"):
            continue
        path = os.path.join(export_dir, m["glb"])
        if not os.path.isfile(path):
            warnings.append("model %s: %s missing" % (m.get("fileId"), m["glb"]))
            continue
        names, acts = import_glb(path)
        arms = bake_objects(alive(names), acts)
        objs = alive(names)
        col = new_collection(str(m.get("fileId")), models_col)
        for o in objs:
            move_to(o, col)
        for a in acts:
            a.use_fake_user = True
        for a in arms:   # the base collection stands at rest; clip copies play
            if a.animation_data:
                a.animation_data.action = None
                for t in list(a.animation_data.nla_tracks):
                    a.animation_data.nla_tracks.remove(t)
        entries[k] = ModelEntry(k, m.get("fileId"), col, {base_name(a.name): a for a in acts})
        mats = materials_of(objs)
        if dark_decal:
            names = set(m.get("darkDecalMaterials") or [])
            for mat in mats:
                if base_name(mat.name) in names:
                    make_dark_decal(mat)
                    n_decal += 1
        for sc in m.get("uvScroll") or []:
            uv_index = int(sc.get("uvIndex", 0))
            for mat in mats:
                if base_name(mat.name) != sc.get("material"):
                    continue
                users = [o for o in objs if o.type == 'MESH' and mat.name in [x.name for x in o.data.materials if x]]
                layers = users[0].data.uv_layers if users else []
                uv_name = layers[uv_index].name if uv_index < len(layers) else ""
                n_scroll += add_uv_scroll(mat, uv_index, sc.get("scroll", [0, 0]), uv_name)

    # Cells.
    cell_info = []
    placed = {}
    skipped = 0
    errors = []
    foliage = []          # (object, records, cell)
    fol_skipped = 0
    for cell in man.get("cells") or []:
        cid = cell["id"]
        ccol = new_collection(cid, cells_col)
        props = new_collection(cid + " Props", ccol)
        ccoll = new_collection(cid + " Colliders", ccol)
        ci = {"id": cid, "terrain": None, "water": None, "colliders": [], "wantTerrain": cell.get("chunks") is not None,
              "wantWater": cell.get("water") is not None}
        if cell.get("glb") and os.path.isfile(os.path.join(export_dir, cell["glb"])):
            names, _acts = import_glb(os.path.join(export_dir, cell["glb"]))
            bake_objects(alive(names), [])
            for o in alive(names):
                b = base_name(o.name)
                if b.startswith("Collider_"):
                    if not colliders:
                        bpy.data.objects.remove(o)
                        continue
                    move_to(o, ccoll)
                    ci["colliders"].append(o)
                else:
                    move_to(o, ccol)
                    if b == "Terrain":
                        ci["terrain"] = o
                    elif b == "Water":
                        ci["water"] = o
                o.name = "%s %s" % (cid, b)
                o.matrix_world = S @ o.matrix_world
        elif cell.get("glb"):
            warnings.append("cell %s: %s missing" % (cid, cell["glb"]))
        ccoll.hide_viewport = True
        ccoll.hide_render = True
        for inst in cell.get("instances") or []:
            entry = entries.get(inst.get("model"))
            if entry is None:
                skipped += 1
                continue
            col = clip_collection(entry, inst["clip"], models_col, warnings) if inst.get("clip") else entry.col
            e = bpy.data.objects.new("%s p%d" % (entry.file_id, inst.get("prop", 0)), None)
            e.instance_type = 'COLLECTION'
            e.instance_collection = col
            e.empty_display_size = 1.0
            props.objects.link(e)
            # Quaternion, not the default XYZ Euler: matrix_world is decomposed into the
            # rotation mode, and XYZ loses up to 1e-2 relative near a 90 degree Y turn
            # (gimbal lock; review I2), QUATERNION about 1e-6.
            e.rotation_mode = 'QUATERNION'
            e.matrix_world = S @ C_B @ matrix_of(inst["m"]) @ C_B
            e["castlemist_cell"] = cid
            e["castlemist_prop"] = int(inst.get("prop", 0))
            placed[(cid, int(inst.get("prop", 0)))] = (e, inst, cell)
        fol = cell.get("foliage")
        if isinstance(fol, dict) and fol.get("file"):
            path = os.path.join(export_dir, fol["file"])
            recs = None
            if not os.path.isfile(path):
                errors.append("foliage: cell %s: missing file %s" % (cid, fol["file"]))
            else:
                try:
                    recs = read_foliage(path)
                except (OSError, ValueError) as ex:
                    errors.append("foliage: cell %s: %s: %s" % (cid, fol["file"], ex))
                if recs is not None and len(recs) != fol.get("records"):
                    errors.append("foliage: cell %s: %s holds %d records, the manifest says %s"
                                  % (cid, fol["file"], len(recs), fol.get("records")))
            by_model = {}
            for r in recs or []:
                if r["model"] in entries:
                    by_model.setdefault(r["model"], []).append(r)
                else:
                    fol_skipped += 1
            if by_model:
                fcol = new_collection(cid + " Foliage", ccol)
                for mi in sorted(by_model):
                    entry = entries[mi]
                    ob = foliage_object("%s %s foliage" % (cid, entry.file_id), fcol, by_model[mi],
                                        float(scale), entry.col)
                    ob["castlemist_cell"] = cid
                    ob["castlemist_foliage_model"] = mi
                    foliage.append((ob, by_model[mi], cell))
        cell_info.append(ci)
    if fol_skipped:
        warnings.append("foliage: %d records of failed or missing models skipped" % fol_skipped)
    for ci in cell_info:
        lcc = layer_collection(bpy.data.collections[ci["id"] + " Colliders"])
        if lcc is not None:
            lcc.hide_viewport = True
    lcm = layer_collection(models_col)
    if lcm is not None:
        lcm.exclude = True

    # Frame probe, spawn, sun, world.
    probe = man.get("frameProbe")
    probe_result = (False, "PROBE FAIL no frameProbe in the manifest", None)
    if probe and probe.get("glb") and os.path.isfile(os.path.join(export_dir, probe["glb"])):
        names, _acts = import_glb(os.path.join(export_dir, probe["glb"]))
        bake_objects(alive(names), [])
        objs = alive(names)
        pcol = new_collection("Frame Probe", root)
        at = S @ C_B @ Vector([float(v) for v in probe.get("map") or (0, 0, 0)])
        for o in objs:
            move_to(o, pcol)
            o.matrix_world = Matrix.Translation(at) @ S @ o.matrix_world
        bpy.context.view_layer.update()
        probe_result = measure_probe(objs, at)
        # Measured: hidden from then on, so a render or view from the spawn is not
        # blocked by the probe (review M6; the Unity importer hides it too).
        for o in objs:
            o.hide_render = True
        pcol.hide_render = True
        lpc = layer_collection(pcol)
        if lpc is not None:
            lpc.hide_viewport = True
    spawn = man.get("spawn")
    if spawn and spawn.get("map"):
        sp = bpy.data.objects.new("Spawn", None)
        sp.empty_display_type = 'SINGLE_ARROW'
        root.objects.link(sp)
        sp.location = S @ C_B @ Vector([float(v) for v in spawn["map"]])
    light = man.get("light")
    sun = (light or {}).get("sun")
    if sun and sun.get("blenderDirection"):
        ld = bpy.data.lights.new("Sun", 'SUN')
        ld.color = [float(c) for c in sun.get("color", (1, 1, 1))][:3]
        ld.energy = float(sun.get("intensity", 1.0))
        so = bpy.data.objects.new("Sun", ld)
        root.objects.link(so)
        d = Vector([float(c) for c in sun["blenderDirection"]]).normalized()
        so.rotation_mode = 'QUATERNION'
        so.rotation_quaternion = d.to_track_quat('-Z', 'Y')   # a Sun lamp shines along its -Z
        if spawn and spawn.get("map"):
            so.location = S @ C_B @ Vector([float(v) for v in spawn["map"]])
    world = bpy.data.worlds.new("GW2 %s" % file_id)
    scene.world = world
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == 'BACKGROUND')
    sky = man.get("sky")
    equirect = sky.get("equirect") if sky else None
    if equirect and os.path.isfile(os.path.join(export_dir, equirect)):
        env = world.node_tree.nodes.new('ShaderNodeTexEnvironment')
        env.image = bpy.data.images.load(os.path.join(export_dir, equirect), check_existing=True)
        world.node_tree.links.new(env.outputs["Color"], bg.inputs["Color"])
    else:
        if equirect:
            warnings.append("sky: %s missing" % equirect)
        amb = (light or {}).get("ambient") or (0.05, 0.05, 0.05)
        bg.inputs["Color"].default_value = [float(c) for c in amb][:3] + [1.0]

    bpy.context.view_layer.update()
    for w in warnings:
        print("IMPORT_WARN " + w)
    for e in errors:
        print("IMPORT_ERROR " + e)
    print("IMPORT models=%d cells=%d instances=%d skipped=%d foliageObjects=%d foliageRecords=%d "
          "foliageSkipped=%d darkDecal=%d uvScroll=%d seconds=%.1f" % (
              len(entries), len(cell_info), len(placed), skipped, len(foliage), sum(len(f[1]) for f in foliage),
              fol_skipped, n_decal, n_scroll, time.time() - t0))
    if not verify:
        return not errors
    return verify_world(man, placed, cell_info, probe_result, colliders, S, foliage, errors)


def verify_foliage(foliage, S, fails):
    """world-foliage D8 --verify: each foliage object's evaluated instances
    (depsgraph.object_instances, through one reference mesh of the model's
    collection) against the records' Unity TRS under the cell origin, as the
    props are checked: 3x3 within 1e-4, translation within 1e-4 max(1, |t|).
    Each record must be matched by exactly one instance."""
    print("FOLIAGE_OBJECTS %d" % len(foliage))
    by_ob = {}
    for ob, recs, cell in foliage:
        col = ob.modifiers[FOLIAGE_GROUP][foliage_collection_key()]
        meshes = sorted((o for o in col.all_objects if o.type == 'MESH'), key=lambda o: o.name)
        if not meshes:
            fails.append("foliage %s: model collection %s has no mesh" % (ob.name, col.name))
            continue
        by_ob[ob.name] = (meshes[0], recs, cell, [])
    dg = bpy.context.evaluated_depsgraph_get()
    for inst in dg.object_instances:
        if not inst.is_instance or inst.parent is None:
            continue
        slot = by_ob.get(inst.parent.original.name)
        if slot is not None and inst.object.original == slot[0]:
            slot[3].append(inst.matrix_world.copy() @ slot[0].matrix_world.inverted())
    n_inst = sum(len(v[3]) for v in by_ob.values())
    print("FOLIAGE_INSTANCES %d" % n_inst)
    want = sum(len(f[1]) for f in foliage)
    if n_inst != want:
        fails.append("foliage instances %d != records %d" % (n_inst, want))
    err_r = err_t = 0.0
    for name, (_ref, recs, cell, got) in by_ob.items():
        origin = C_B @ Vector([float(v) for v in cell["origin"]])
        exp = []
        kd = KDTree(len(recs))
        for k, r in enumerate(recs):
            t, q, s = r["unity"]["t"], r["unity"]["r"], float(r["unity"]["s"])
            U = Matrix.Translation(Vector(t)) @ Quaternion((q[3], q[0], q[1], q[2])).to_matrix().to_4x4() @ Matrix.Scale(s, 4)
            B = Matrix.Scale(S, 4) @ Matrix.Translation(origin) @ P_UB @ U @ P_UB
            exp.append(B)
            kd.insert(B.translation, k)
        kd.balance()
        hit = [0] * len(recs)
        for M in got:
            _co, k, _d = kd.find(M.translation)
            hit[k] += 1
            B = exp[k]
            for i in range(3):
                for j in range(3):
                    err_r = max(err_r, abs(M[i][j] - B[i][j]))
                err_t = max(err_t, abs(M[i][3] - B[i][3]) / max(1.0, abs(B[i][3])))
        if any(h != 1 for h in hit):
            fails.append("foliage %s: %d records not matched by exactly one instance"
                         % (name, sum(1 for h in hit if h != 1)))
    print("FOLIAGE_MATRIX_MAX_ERR rot=%.3g t=%.3g" % (err_r, err_t))
    if not (err_r < 1e-4 and err_t < 1e-4):
        fails.append("foliage matrix error rot=%.3g t=%.3g" % (err_r, err_t))


def verify_world(man, placed, cell_info, probe_result, colliders, S, foliage=(), errors=()):
    """The headless checks of spec "Testing" (Blender). The instance matrices are
    checked against the manifest's other form of each placement, its Unity TRS
    under the cell origin (t, r, s; map_to_unity frame): S T(C_b origin) P U P,
    P the Unity -> Blender axis swap -- not against the matrix the importer used."""
    fails = []
    want = sum(len(c.get("instances") or []) for c in man.get("cells") or [])
    empties = [o for o in bpy.data.objects if o.instance_type == 'COLLECTION' and o.instance_collection is not None
               and "castlemist_prop" in o]
    print("INSTANCES %d" % len(empties))
    if len(empties) != want:
        fails.append("instances %d != manifest %d" % (len(empties), want))
    err = 0.0
    for (_cid, _prop), (e, inst, cell) in placed.items():
        u = inst.get("unity")
        if not u:
            fails.append("instance without unity TRS")
            continue
        t, r, s = u["t"], u["r"], float(u["s"])
        U = Matrix.Translation(Vector(t)) @ Quaternion((r[3], r[0], r[1], r[2])).to_matrix().to_4x4() @ Matrix.Scale(s, 4)
        origin = C_B @ Vector([float(v) for v in cell["origin"]])
        B = S @ Matrix.Translation(origin) @ P_UB @ U @ P_UB
        M = e.matrix_world
        for i in range(4):
            for j in range(4):
                err = max(err, abs(M[i][j] - B[i][j]) / max(1.0, abs(B[i][j])))
    print("MATRIX_MAX_ERR %.3g" % err)
    if not err < 1e-4:
        fails.append("matrix error %.3g" % err)
    ok, line, ext = probe_result
    print(line)
    if ext:
        print(ext)
    if not ok:
        fails.append("probe")
    want_t = sum(1 for c in cell_info if c["wantTerrain"])
    n_t = sum(1 for c in cell_info if c["terrain"] is not None and
              any(image_ok(n.image) for m in materials_of([c["terrain"]]) for n in image_nodes_of(m)))
    print("TERRAIN_IMAGES %d" % n_t)
    if n_t != want_t:
        fails.append("terrain images %d of %d" % (n_t, want_t))
    want_w = sum(1 for c in cell_info if c["wantWater"])
    n_w = sum(1 for c in cell_info if c["water"] is not None and
              any(image_ok(n.image) for m in materials_of([c["water"]]) for n in image_nodes_of(m)))
    print("WATER_IMAGES %d" % n_w)
    if n_w != want_w:
        fails.append("water images %d of %d" % (n_w, want_w))
    cols = [o for c in cell_info for o in c["colliders"]]
    hidden = all(not o.visible_get() and o.users_collection[0].hide_render for o in cols)
    print("COLLIDERS %d" % len(cols))
    print("COLLIDERS_HIDDEN %d" % (1 if hidden else 0))
    if not hidden:
        fails.append("colliders visible")
    want_c = sum(int(c.get("colliderParts") or 0) for c in man.get("cells") or []) if colliders else 0
    if len(cols) != want_c:
        fails.append("colliders %d != %d" % (len(cols), want_c))
    verify_foliage(foliage, S[0][0], fails)
    fails.extend(errors)
    print("VERIFY OK" if not fails else "VERIFY FAIL " + "; ".join(fails))
    return not fails


def main():
    if bpy.app.version < (4, 1, 0):   # Mesh.corner_normals and the glTF importer's behaviour (review M4)
        print("castlemist_world.py needs Blender 4.1 or newer; this is %s" % bpy.app.version_string)
        sys.exit(2)
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        raise SystemExit(__doc__)
    ok = True
    i = 0
    while i < len(args):
        cmd = args[i]
        if cmd in ("--to-fbx", "--check-probe", "--check-skinned-probe", "--import") and i + 1 >= len(args):
            raise SystemExit(cmd + " needs a path")
        if cmd == "--to-fbx":
            ok = to_fbx(args[i + 1]) and ok
            i += 2
        elif cmd == "--check-probe":
            ok = check_probe(args[i + 1]) and ok
            i += 2
        elif cmd == "--check-skinned-probe":
            ok = check_skinned_probe(args[i + 1]) and ok
            i += 2
        elif cmd == "--import":
            opts = {"colliders": True, "dark_decal": True, "scale": 1.0, "verify": False}
            save = None
            export_dir = args[i + 1]
            i += 2
            while i < len(args) and args[i] in ("--no-colliders", "--no-dark-decal", "--scale", "--verify", "--save"):
                a = args[i]
                if a == "--no-colliders":
                    opts["colliders"] = False
                elif a == "--no-dark-decal":
                    opts["dark_decal"] = False
                elif a == "--verify":
                    opts["verify"] = True
                elif a in ("--scale", "--save"):
                    if i + 1 >= len(args):
                        raise SystemExit(a + " needs a value")
                    if a == "--scale":
                        opts["scale"] = float(args[i + 1])
                        if not opts["scale"] > 0 or not math.isfinite(opts["scale"]):
                            raise SystemExit("--scale must be positive")
                    else:
                        save = args[i + 1]
                    i += 1
                i += 1
            ok = import_world(export_dir, **opts) and ok
            if save:
                bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(save))
        else:
            raise SystemExit("unknown argument " + cmd)
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
