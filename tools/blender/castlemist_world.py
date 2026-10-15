"""castlemist world export -> Blender / FBX (sub-project 4).

Headless:
    blender -b --factory-startup -P castlemist_world.py -- <command> [<command> ...]

Commands run in order, in one Blender session:
    --to-fbx <exportDir>          every .glb under <exportDir> (probe.glb, cells/*.glb,
                                  models/*/*.glb) to a .fbx beside it; files whose .fbx
                                  is newer than the .glb are skipped
    --check-probe <file.glb|fbx>  imports the frame probe into an empty scene and prints
                                  PROBE east=(x,y,z) north=(..) up=(..) negScale=<0|1> OK
                                  or ... FAIL <axis>

Spec: docs/superpowers/specs/2026-10-15-world-export-design.md D6 ("Blender",
"Frame probe"). The expected directions are exportgltf::frame_probe_expect()
(include/castlemist/exportgltf/world_probe.h), from world/frame.h's
map_to_blender: east +X, north +Y, up +Z, bar lengths 3 : 2 : 1.

FBX axis settings (Task 1 frame probe, measured 2026-10-15 with Blender 4.5 and
Unity 6000.0.42f1): the VRChat script's model mode, axis_forward='-Z',
axis_up='Y'. Unity honours the axis system the FBX declares, so axis_forward
'-Z' and 'Z' import to the same world result there. What decides Unity's
result is ModelImporter.bakeAxisConversion: off, east and north come out as
-X and -Z (a 180 degree turn about Y); on, they come out as +X and +Z, as
map_to_unity says. The importer sets it on (FrameProbe.BakeAxisConversion).
"""
import os
import sys

import bpy
from mathutils import Vector

# Blender's -Y (its "forward") -> FBX -Z: castlemist_vrchat.py's setting.
FBX_AXIS_FORWARD = '-Z'
FBX_AXIS_UP = 'Y'

PROBE_EXPECT = {"east": (1.0, 0.0, 0.0), "north": (0.0, 1.0, 0.0), "up": (0.0, 0.0, 1.0)}
PROBE_BARS = {"east": "Probe_East", "north": "Probe_North", "up": "Probe_Up"}
PROBE_LENGTHS = (3.0, 2.0, 1.0)
PROBE_ROOTS = ("GW2_MapToGltf", "probe")


def empty_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_any(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    else:
        raise SystemExit("cannot import " + path)


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


def inward_faces(ob, centre):
    """Faces whose world-space winding normal, or any of whose corner (shading)
    normals, point into the box: a reflection applied without flipping the
    winding shows up here."""
    m = ob.matrix_world
    n3 = m.to_3x3().inverted_safe().transposed()
    # Under det < 0 a face's winding normal is the negated n3 image.
    flip = -1.0 if m.determinant() < 0 else 1.0
    me = ob.data
    bad = 0
    for p in me.polygons:
        out = m @ p.center - centre
        if flip * (n3 @ p.normal).dot(out) <= 0:
            bad += 1
        # The shading (custom split) normals the FBX carries to Unity.
        for li in p.loop_indices:
            if (n3 @ me.corner_normals[li].vector).dot(out) <= 0:
                bad += 1
    return bad


def check_probe(path):
    """Imports the probe as the pipeline does and measures it (world_probe.h's
    check rule). A .glb is imported raw -- the reflected root leaves every
    object at det < 0, printed as PROBE_RAW -- and then gets bake_mesh_only(),
    the same step --to-fbx and the importer apply, before it is measured."""
    empty_scene()
    import_any(path)
    bpy.context.view_layer.update()
    objs = list(bpy.context.scene.objects)
    root = next((o for o in objs if o.name in PROBE_ROOTS), None)
    origin = root.matrix_world.translation.copy() if root else Vector((0.0, 0.0, 0.0))
    if os.path.splitext(path)[1].lower() in (".glb", ".gltf"):
        print("PROBE_RAW negScale=%d objects=%d" % (neg_scale(objs), len(objs)))
        bake_mesh_only()
        bpy.context.view_layer.update()
        objs = list(bpy.context.scene.objects)
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
    line += " OK" if not fails else " FAIL " + ",".join(fails)
    print(line)
    if extents:
        print("PROBE_EXTENTS " + " ".join("%s=%.4f" % (k, v) for k, v in extents.items()))
    return not fails and neg == 0


# ------------------------------------------------------------------ glb -> fbx --

def is_animated(ob):
    ad = ob.animation_data
    return ad is not None and (ad.action is not None or len(ad.nla_tracks) > 0)


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


def bake_mesh_only():
    """After a .glb import: a mesh-only file gets every transform (the reflected
    GW2_MapToGltf root included) applied into its vertex data, and its empties
    removed, so no object is left with det < 0. A file with an armature keeps
    its transforms, as castlemist_vrchat.py does. Returns the armature or None."""
    objs = list(bpy.context.scene.objects)
    arm = next((o for o in objs if o.type == 'ARMATURE'), None)
    if arm is not None:
        return arm
    meshes = [o for o in objs if o.type == 'MESH']
    if meshes:
        mirrored = [o for o in meshes if o.matrix_world.determinant() < 0]
        for o in meshes:
            if o.parent is not None:
                world = o.matrix_world.copy()
                o.parent = None
                o.matrix_world = world
        apply_transforms(meshes)
        # Blender 4.5's transform_apply moves the vertices through a reflection
        # but keeps the winding (measured by the Task 1 probe: every face and
        # corner normal ends up pointing inward). Flip those meshes back;
        # Mesh.flip_normals also flips custom split normals.
        for o in mirrored:
            o.data.flip_normals()
    for o in [o for o in bpy.context.scene.objects if o.type == 'EMPTY']:
        bpy.data.objects.remove(o)
    return None


def glb_to_fbx(src, out):
    empty_scene()
    bpy.ops.import_scene.gltf(filepath=src)
    arm = bake_mesh_only()
    animated = arm is not None and is_animated(arm)
    bpy.ops.export_scene.fbx(
        filepath=out,
        use_selection=False,
        object_types={'ARMATURE', 'MESH'},
        apply_scale_options='FBX_SCALE_ALL',
        apply_unit_scale=True,
        axis_forward=FBX_AXIS_FORWARD,
        axis_up=FBX_AXIS_UP,
        bake_space_transform=animated,
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
        path_mode='RELATIVE',
        embed_textures=False,
    )


def to_fbx(export_dir):
    done = skipped = 0
    for dirpath, _dirs, files in os.walk(export_dir):
        for f in sorted(files):
            if not f.lower().endswith(".glb"):
                continue
            src = os.path.join(dirpath, f)
            out = os.path.splitext(src)[0] + ".fbx"
            if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
                skipped += 1
                continue
            glb_to_fbx(src, out)
            done += 1
    print("TO_FBX converted=%d skipped=%d" % (done, skipped))


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        raise SystemExit(__doc__)
    ok = True
    i = 0
    while i < len(args):
        cmd = args[i]
        if cmd in ("--to-fbx", "--check-probe") and i + 1 >= len(args):
            raise SystemExit(cmd + " needs a path")
        if cmd == "--to-fbx":
            to_fbx(args[i + 1])
        elif cmd == "--check-probe":
            ok = check_probe(args[i + 1]) and ok
        else:
            raise SystemExit("unknown argument " + cmd)
        i += 2
    if not ok:
        sys.exit(1)


main()
