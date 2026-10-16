"""Headless check of an .fbx written by tools/blender/castlemist_vrchat.py.

    blender -b --factory-startup -P tests/blender/check_fbx.py -- <file.fbx>

Prints, one per line:
    ACTIONS <name> <keyframes> <moving 0|1>   (one line per action)
    MESHES <n>
    MATERIALS <n>
    ARMATURE <0|1>
    IMAGES <n> missing=<m>       (distinct image files the materials use; missing = not found)
    NEGSCALE <0|1> INWARD <n>    (an object with det < 0; faces / corner normals
                                  pointing into their object's bounds centre --
                                  meaningful for convex test models only)
<keyframes> is the most keyframes on any of the action's curves; <moving> is 1
when any curve's values differ by more than 1e-4 across its keys.
"""
import os
import sys

import bpy
from mathutils import Vector


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


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        raise SystemExit("usage: blender -b -P check_fbx.py -- <file.fbx>")
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=args[0])

    for action in sorted(bpy.data.actions, key=lambda a: a.name):
        keys = 0
        moving = 0
        for fc in curves_of(action):
            pts = fc.keyframe_points
            keys = max(keys, len(pts))
            if len(pts) > 1:
                vals = [p.co[1] for p in pts]
                if max(vals) - min(vals) > 1e-4:
                    moving = 1
        print("ACTIONS", action.name, keys, moving)
    objs = bpy.context.scene.objects
    print("MESHES", sum(1 for o in objs if o.type == 'MESH'))
    print("MATERIALS", len(bpy.data.materials))
    print("ARMATURE", 1 if any(o.type == 'ARMATURE' for o in objs) else 0)

    images = set()
    for m in bpy.data.materials:
        if m.use_nodes and m.node_tree:
            images.update(n.image for n in m.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image)
    # Distinct files: the FBX importer may load one file into several images.
    files = set(os.path.normcase(os.path.normpath(bpy.path.abspath(i.filepath))) for i in images if not i.packed_file)
    missing = [f for f in files if not os.path.isfile(f)]
    print("IMAGES %d missing=%d" % (len(files), len(missing)))
    for f in missing:
        print("IMAGE_MISSING", f)

    neg = 1 if any(o.matrix_world.determinant() < 0 for o in objs) else 0
    inward = 0
    for o in objs:
        if o.type != 'MESH':
            continue
        m = o.matrix_world
        n3 = m.to_3x3().inverted_safe().transposed()
        flip = -1.0 if m.determinant() < 0 else 1.0
        pts = [m @ Vector(c) for c in o.bound_box]
        centre = sum(pts, Vector()) / len(pts)
        me = o.data
        for p in me.polygons:
            out = m @ p.center - centre
            if flip * (n3 @ p.normal).dot(out) <= 0:
                inward += 1
            for li in p.loop_indices:
                if (n3 @ me.corner_normals[li].vector).dot(out) <= 0:
                    inward += 1
    print("NEGSCALE %d INWARD %d" % (neg, inward))


main()
