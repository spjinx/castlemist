"""Headless check of castlemist_world.py's glb -> fbx step on a static skinned model.

    blender -b --factory-startup -P tests/blender/check_static_skin.py --         <tools/blender/castlemist_world.py> <scratch dir>

Many GW2 props are skinned to one joint whose bind pose carries a scale (map
506592's rock 90608: 3280.66), cancelled by the skin's inverse bind matrix. A
Blender bone's rest pose cannot hold a scale, so the mesh is stored at 1 / scale
and the rig's pose puts it back: the .fbx must keep the armature and the skin
(the Unity importer imports such models with their rig, WorldImporter.NeedsRig).

Writes a .glb like the world export's (mirrored GW2_MapToGltf root, one 10 x 20
x 30 box off the origin, skinned to joint "Scaled" at scale 1000, no animations)
and the same box without a skin, converts both, re-imports each .fbx and
measures the meshes' world bounds as their rig deforms them. The off-centre box
shows a flip or a mirror, which a centred one would hide. Prints STATIC_SKIN
lo=<x,y,z> hi=<x,y,z> plainLo=... plainHi=... skinned=<0|1> OK | FAIL (OK: the
skinned box, deformed, has the plain box's bounds and is still skinned).
"""
import importlib.util
import json
import os
import struct
import sys

import bpy
from mathutils import Vector

S = 1000.0
BOX = (10.0, 20.0, 30.0)
OFF = (5.0, 40.0, -70.0)   # the box's min corner: nothing symmetric about any axis


def box_glb(path, skinned=True):
    """The box, glTF frame, as the world export writes a skinned model (or the
    same nodes with a plain mesh)."""
    pos, idx = [], []
    for i in range(8):
        pos.append(tuple(OFF[k] + (BOX[k] if (i >> k) & 1 else 0.0) for k in range(3)))
    for a, b, c, d in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        idx += [a, b, c, a, c, d]
    blob = b"".join(struct.pack("<3f", *p) for p in pos)
    o_idx = len(blob)
    blob += b"".join(struct.pack("<H", i) for i in idx)
    blob += b"\0" * (-len(blob) % 4)
    o_j = len(blob)
    blob += struct.pack("<4B", 1, 0, 0, 0) * 8
    o_w = len(blob)
    blob += struct.pack("<4f", 1, 0, 0, 0) * 8
    o_ibm = len(blob)
    ident = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    inv = (1 / S, 0, 0, 0, 0, 1 / S, 0, 0, 0, 0, 1 / S, 0, 0, 0, 0, 1)
    blob += struct.pack("<16f", *ident) + struct.pack("<16f", *inv)
    views = [(0, o_idx, 34962), (o_idx, len(idx) * 2, 34963), (o_j, 32, 34962), (o_w, 128, 34962),
             (o_ibm, 128, None)]
    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [3]}],
        "nodes": [
            {"name": "ArenaExport", "children": [1]},
            {"name": "Scaled", "scale": [S, S, S]},
            dict({"name": "box_0", "mesh": 0}, **({"skin": 0} if skinned else {})),
            {"name": "GW2_MapToGltf", "children": [2, 0],
             "matrix": [1, 0, 0, 0, 0, 0, -1, 0, 0, -1, 0, 0, 0, 0, 0, 1]},
        ],
        "meshes": [{"primitives": [{"attributes": dict({"POSITION": 0}, **({"JOINTS_0": 2, "WEIGHTS_0": 3} if skinned else {})),
                                    "indices": 1}]}],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [dict({"buffer": 0, "byteOffset": o, "byteLength": n}, **({"target": t} if t else {}))
                        for o, n, t in views],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3",
             "min": list(OFF), "max": [o + b for o, b in zip(OFF, BOX)]},
            {"bufferView": 1, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
            {"bufferView": 2, "componentType": 5121, "count": 8, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5126, "count": 8, "type": "VEC4"},
            {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"},
        ],
    }
    if skinned:
        gltf["skins"] = [{"joints": [0, 1], "inverseBindMatrices": 4}]
    js = json.dumps(gltf).encode()
    js += b" " * (-len(js) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(blob)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + blob)


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(args) != 2:
        raise SystemExit("usage: check_static_skin.py -- <castlemist_world.py> <scratch>")
    spec = importlib.util.spec_from_file_location("castlemist_world", args[0])
    cw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cw)
    os.makedirs(args[1], exist_ok=True)

    def convert(name, skinned):
        glb = os.path.join(args[1], name + ".glb")
        fbx = os.path.join(args[1], name + ".fbx")
        box_glb(glb, skinned)
        cw.glb_to_fbx(glb, fbx)
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.fbx(filepath=fbx)
        lo = Vector((1e30, 1e30, 1e30))
        hi = -lo
        dg = bpy.context.evaluated_depsgraph_get()
        skinned = 0
        for o in bpy.context.scene.objects:
            if o.type != 'MESH':
                continue
            if any(m.type == 'ARMATURE' and m.object is not None for m in o.modifiers) and o.vertex_groups:
                skinned += 1
            ev = o.evaluated_get(dg)   # as the rig deforms it
            me = ev.to_mesh()
            for v in me.vertices:
                p = ev.matrix_world @ v.co
                lo = Vector(map(min, lo, p))
                hi = Vector(map(max, hi, p))
            ev.to_mesh_clear()
        return lo, hi, skinned

    plo, phi, _ = convert("static_plain", False)
    lo, hi, skinned = convert("static_skin", True)
    ok = skinned == 1 and (lo - plo).length < 1e-3 and (hi - phi).length < 1e-3
    f = lambda v: ",".join("%.4g" % c for c in v)
    print("STATIC_SKIN lo=%s hi=%s plainLo=%s plainHi=%s skinned=%d %s"
          % (f(lo), f(hi), f(plo), f(phi), skinned, "OK" if ok else "FAIL"))


if __name__ == "__main__":
    main()
