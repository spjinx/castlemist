"""Headless check of an .fbx written by tools/blender/castlemist_vrchat.py.

    blender -b --factory-startup -P tests/blender/check_fbx.py -- <file.fbx>

Prints, one per line:
    ACTIONS <name> <keyframes> <moving 0|1>   (one line per action)
    MESHES <n>
    MATERIALS <n>
    ARMATURE <0|1>
<keyframes> is the most keyframes on any of the action's curves; <moving> is 1
when any curve's values differ by more than 1e-4 across its keys.
"""
import sys

import bpy


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


main()
