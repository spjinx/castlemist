"""Headless check of castlemist_world.py --import's material switches, on a copy
of the synthetic world export whose manifest names a dark-decal material and a
UV scroll, and a sky with an equirect (the synthetic export itself has none).

    blender -b --factory-startup -P tests/blender/check_world_extras.py -- \
        <tools/blender/castlemist_world.py> <fixture dir> <scratch dir>

<scratch dir> is replaced by the copy. Prints DARK_DECAL <n>, UV_SCROLL <u> <v>
(the Mapping location at one second), SKY <image> and SUN <direction>, then
EXTRAS OK, or EXTRAS FAIL <why>.
"""
import importlib.util
import json
import os
import shutil
import sys

import bpy
from mathutils import Vector


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(args) != 3:
        raise SystemExit("usage: check_world_extras.py -- <castlemist_world.py> <fixture> <scratch>")
    script, fixture, scratch = args
    spec = importlib.util.spec_from_file_location("castlemist_world", script)
    cw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cw)

    shutil.rmtree(scratch, ignore_errors=True)
    shutil.copytree(fixture, scratch, ignore=shutil.ignore_patterns("*.fbx", "blender.log"))
    path = os.path.join(scratch, "manifest.json")
    with open(path, "rb") as f:
        man = json.loads(f.read().decode("utf-8"))
    mat = "Mat_58246"   # model 101's one material
    man["models"][0]["darkDecalMaterials"] = [mat]
    man["models"][0]["uvScroll"] = [{"material": mat, "uvIndex": 0, "scroll": [0.25, 0.5]}]
    # A 4 x 2 equirect where write_skybox puts the day mode's (spec D5).
    sky_rel = "sky/day/baked/equirect.png"
    os.makedirs(os.path.join(scratch, os.path.dirname(sky_rel)), exist_ok=True)
    img = bpy.data.images.new("equirect_fixture", 4, 2)
    img.filepath_raw = os.path.join(scratch, sky_rel)
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)
    man["sky"] = {"json": "sky/sky.json", "default": "day", "equirect": sky_rel}
    with open(path, "wb") as f:
        f.write(json.dumps(man, indent=2).encode("utf-8"))

    fails = []
    if not cw.import_world(scratch, verify=True):
        fails.append("verify")
    mats = [m for m in bpy.data.materials if cw.base_name(m.name) == mat]
    blended = [m for m in mats if getattr(m, "blend_method", "BLEND") == 'BLEND'
               and getattr(m, "surface_render_method", "BLENDED") == 'BLENDED']
    print("DARK_DECAL %d of %d" % (len(blended), len(mats)))
    if not mats or len(blended) != len(mats):
        fails.append("dark decal")
    # Model 102's material is not named, so it stays as imported.
    other = [m for m in bpy.data.materials if cw.base_name(m.name) == "Mat_58247"]
    if any(getattr(m, "surface_render_method", "") == 'BLENDED' for m in other):
        fails.append("dark decal on an unnamed material")

    scene = bpy.context.scene
    env = [n for n in scene.world.node_tree.nodes if n.type == 'TEX_ENVIRONMENT'] if scene.world else []
    sky_img = env[0].image if env else None
    print("SKY %s" % (sky_img.filepath if sky_img else "none"))
    sky_ok = sky_img is not None and env[0].outputs["Color"].is_linked and         os.path.samefile(bpy.path.abspath(sky_img.filepath), os.path.join(scratch, sky_rel))
    if not sky_ok:
        fails.append("sky")
    sun = bpy.data.objects.get("Sun")
    want = Vector(man["light"]["sun"]["blenderDirection"]).normalized()
    got = (sun.matrix_world.to_3x3() @ Vector((0, 0, -1))).normalized() if sun else Vector()
    print("SUN (%.3f,%.3f,%.3f)" % tuple(got))
    if sun is None or sun.data.type != 'SUN' or got.dot(want) < 0.9999:
        fails.append("sun")
    if got.z >= 0:   # blenderDirection is the direction the light travels: down onto the map
        fails.append("sun points up")

    scene.frame_set(scene.render.fps)   # t = 1 s
    maps = [n for m in mats for n in m.node_tree.nodes if n.type == 'MAPPING']
    if not maps:
        fails.append("no mapping node")
    for n in maps:
        loc = n.inputs["Location"].default_value
        print("UV_SCROLL %.4f %.4f" % (loc[0], loc[1]))
        # glTF V is Blender's 1 - V, so V scrolls with the opposite sign.
        if abs(loc[0] - 0.25) > 1e-4 or abs(loc[1] + 0.5) > 1e-4:
            fails.append("scroll at 1 s")
        img = n.outputs["Vector"].links[0].to_node if n.outputs["Vector"].links else None
        if img is None or img.type != 'TEX_IMAGE':
            fails.append("mapping not feeding the image")
    scene.frame_set(0)
    for n in maps:
        if abs(n.inputs["Location"].default_value[0]) > 1e-6:
            fails.append("scroll at 0 s")
    # --no-dark-decal: the same manifest, the material left as imported.
    cw.import_world(scratch, dark_decal=False)
    mats = [m for m in bpy.data.materials if cw.base_name(m.name) == mat]
    if not mats or any(getattr(m, "surface_render_method", "") == 'BLENDED' for m in mats):
        fails.append("--no-dark-decal")
    print("EXTRAS OK" if not fails else "EXTRAS FAIL " + ", ".join(fails))


main()
