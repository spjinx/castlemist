"""castlemist -> VRChat: turn a castlemist VRChat export (.glb) into the .fbx Unity takes.

Headless (castlemist runs this itself when it finds Blender):
    blender -b --factory-startup -P castlemist_vrchat.py -- <in.glb> <out.fbx>
Or interactively: Scripting tab > open this file > set GLB / FBX below > Run Script.

What it does
  * imports the .glb (bones already carry Unity humanoid names: Hips, Spine, ...)
  * applies the root rotation / scale, so the armature and meshes are at identity
  * joins every mesh into one object, "Body" (one skinned mesh is what VRChat's
    performance ranking wants); shape keys -- face details, Blink, vrc.v_* -- survive
  * names the armature "Armature"
  * exports .fbx for Unity (Y up, -Z forward, no leaf bones, textures embedded) and
    saves a .blend beside it for further editing
"""
import os
import sys

import bpy

GLB = ""  # interactive use: path to the castlemist .glb
FBX = ""  # interactive use: where to write the .fbx


def convert(src, out):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=src, bone_heuristic='TEMPERANCE')

    arm = next((o for o in bpy.context.scene.objects if o.type == 'ARMATURE'), None)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not arm or not meshes:
        raise SystemExit("no armature / meshes in " + src)

    # Root rotation and scale onto the data: Unity wants the armature at identity.
    bpy.ops.object.select_all(action='DESELECT')
    for o in [arm] + meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    # One skinned mesh.
    bpy.ops.object.select_all(action='DESELECT')
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    body = bpy.context.view_layer.objects.active
    body.name = body.data.name = "Body"
    if body.parent != arm:
        body.parent = arm
    if not any(m.type == 'ARMATURE' for m in body.modifiers):
        mod = body.modifiers.new("Armature", 'ARMATURE')
        mod.object = arm
    arm.name = arm.data.name = "Armature"

    # Stray empties (the importer's root node, if any) out of the way.
    for o in list(bpy.context.scene.objects):
        if o.type == 'EMPTY' and not o.children:
            bpy.data.objects.remove(o)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.splitext(out)[0] + ".blend")
    bpy.ops.export_scene.fbx(
        filepath=out,
        use_selection=False,
        object_types={'ARMATURE', 'MESH'},
        apply_scale_options='FBX_SCALE_ALL',
        apply_unit_scale=True,
        axis_forward='-Z',
        axis_up='Y',
        add_leaf_bones=False,
        primary_bone_axis='Y',
        secondary_bone_axis='X',
        use_armature_deform_only=False,
        use_mesh_modifiers=False,  # keeps the shape keys
        mesh_smooth_type='FACE',
        bake_anim=False,
        path_mode='COPY',
        embed_textures=True,
    )
    print("CASTLEMIST_VRCHAT_OK", out)


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    src = args[0] if len(args) > 0 else GLB
    out = args[1] if len(args) > 1 else (FBX or os.path.splitext(src)[0] + ".fbx")
    if not src:
        raise SystemExit("usage: blender -b -P castlemist_vrchat.py -- <in.glb> [out.fbx]")
    convert(src, out)
