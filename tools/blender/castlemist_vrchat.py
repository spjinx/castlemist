"""castlemist -> VRChat: turn a castlemist VRChat export (.glb) into the .fbx Unity takes.

Headless (castlemist runs this itself when it finds Blender):
    blender -b --factory-startup -P castlemist_vrchat.py -- <in.glb> <out.fbx> [--mode model|character]
Or interactively: Scripting tab > open this file > set GLB / FBX / MODE below > Run Script.

What it does
  * imports the .glb (a character's bones already carry Unity humanoid names: Hips, Spine, ...)
  * keeps every animation clip: each imported action becomes its own FBX take
    (a Unity clip, named "Armature|<GW2 clip name>")
  * an armature with no animation: applies the root rotation / scale, so the
    armature and meshes are at identity (as before). An animated armature keeps
    its transforms -- applying them would leave every action's curves in the old
    space -- and the FBX export bakes the axis change instead.
  * a model with no armature (props): exports its meshes as plain meshes
  * character mode only (the default, used by the Character Ripper):
      - joins each avatar piece into one object -- Body (bare body + head), Hair, and
        each armor piece / back item / weapon on its own, so they can be toggled;
        shape keys -- face details, Blink, vrc.v_* -- survive; UV sets are named UVMap
        and UVDiscard (Poiyomi UV Tile Discard) on every piece before joining
      - points the eye bones up with no roll (VRChat: Y up, Z forward)
  * model mode joins nothing: every mesh stays as castlemist named it
  * names the armature "Armature"
  * exports .fbx for Unity (Y up, -Z forward, no leaf bones, textures embedded) and
    saves a .blend beside it for further editing
"""
import os
import sys

import bpy

GLB = ""  # interactive use: path to the castlemist .glb
FBX = ""  # interactive use: where to write the .fbx
MODE = "character"  # interactive use: "character" or "model"


def is_animated(ob):
    ad = ob.animation_data
    return ad is not None and (ad.action is not None or len(ad.nla_tracks) > 0)


def apply_transforms(objs):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def join_pieces(arm, meshes):
    """Character mode: one object per avatar piece, UV sets UVMap + UVDiscard."""
    # castlemist names meshes "<piece>_<n>" (Body, Hair, Coat, Gloves, ...); the
    # parts of a piece join, pieces stay apart so they can be toggled.
    # The same UV sets, same names, on every mesh before joining (Blender joins
    # UV layers by name): UVMap, and UVDiscard (UV1, Poiyomi UV Tile Discard).
    for o in meshes:
        layers = o.data.uv_layers
        if len(layers) == 0:
            layers.new(name="UVMap")
        layers[0].name = "UVMap"
        if len(layers) < 2:
            layers.new(name="UVDiscard", do_init=True)
        layers[1].name = "UVDiscard"
        while len(layers) > 2:
            layers.remove(layers[2])

    groups = {}
    for o in meshes:
        piece = o.name.rsplit("_", 1)[0] if "_" in o.name else o.name
        groups.setdefault(piece, []).append(o)
    for piece, objs in groups.items():
        bpy.ops.object.select_all(action='DESELECT')
        for o in objs:
            o.select_set(True)
        bpy.context.view_layer.objects.active = objs[0]
        if len(objs) > 1:
            bpy.ops.object.join()
        joined = bpy.context.view_layer.objects.active
        joined.name = joined.data.name = piece
        if arm is None:
            continue
        if joined.parent != arm:
            world = joined.matrix_world.copy()
            joined.parent = arm
            joined.matrix_world = world  # an animated armature is not at identity
        if not any(m.type == 'ARMATURE' for m in joined.modifiers):
            mod = joined.modifiers.new("Armature", 'ARMATURE')
            mod.object = arm


def eye_bones_up(arm):
    """Eye bones as VRChat wants them: pointing up with no roll (Y up, Z forward in Unity)."""
    bpy.ops.object.select_all(action='DESELECT')
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    for name in ("LeftEye", "RightEye"):
        b = arm.data.edit_bones.get(name)
        if b:
            b.tail = b.head.copy()
            b.tail.z += 0.03
            b.roll = 0.0
    bpy.ops.object.mode_set(mode='OBJECT')


def convert(src, out, mode="character"):
    if mode not in ("character", "model"):
        raise SystemExit("unknown --mode " + mode + " (character or model)")
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=src, bone_heuristic='TEMPERANCE')

    arm = next((o for o in bpy.context.scene.objects if o.type == 'ARMATURE'), None)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not meshes:
        raise SystemExit("no meshes in " + src)

    # Meshes hanging off an empty (the importer's root node when there is no
    # armature) come off it where they are: empties are not exported.
    for o in meshes:
        if o.parent is not None and o.parent != arm:
            world = o.matrix_world.copy()
            o.parent = None
            o.matrix_world = world

    # Stray empties (the importer's root node and the node chain above the
    # meshes, if any) out of the way, so their names are free for the pieces.
    stray = True
    while stray:
        stray = [o for o in bpy.context.scene.objects if o.type == 'EMPTY' and not o.children]
        for o in stray:
            bpy.data.objects.remove(o)

    animated = arm is not None and is_animated(arm)
    if arm is None:
        # A prop: the root rotation / scale onto the mesh data.
        apply_transforms(meshes)
    elif not animated:
        # Root rotation and scale onto the data: Unity wants the armature at identity.
        apply_transforms([arm] + meshes)
    # An animated armature keeps its transforms (and so do its meshes): applying
    # them would leave every action's curves in the old space. The FBX export
    # bakes the axis change (bake_space_transform) instead.

    if mode == "character":
        join_pieces(arm, meshes)
    if arm is not None:
        arm.name = arm.data.name = "Armature"
        if mode == "character":
            eye_bones_up(arm)

    # Every material's images as their own files: the glTF importer leaves them
    # packed with no path, and the FBX exporter tells textures apart by path --
    # unsaved ones collapse onto one image (the body showed the armor atlas).
    tex_dir = os.path.join(os.path.dirname(out), os.path.splitext(os.path.basename(out))[0] + " Textures", "fbx")
    os.makedirs(tex_dir, exist_ok=True)
    saved = {}
    for mat in bpy.data.materials:
        if not mat.use_nodes:
            continue
        for node in mat.node_tree.nodes:
            if node.type != 'TEX_IMAGE' or not node.image:
                continue
            img = node.image
            if img.name not in saved:
                safe = "".join(c if c.isalnum() or c in " -_" else "_" for c in mat.name + " " + node.label + " " + img.name)
                path = os.path.join(tex_dir, safe.strip() + ".png")
                img.file_format = 'PNG'
                img.save(filepath=path)
                loaded = bpy.data.images.load(path, check_existing=False)
                loaded.colorspace_settings.name = img.colorspace_settings.name
                loaded.alpha_mode = img.alpha_mode
                saved[img.name] = loaded
            node.image = saved[img.name]

    bpy.ops.wm.save_as_mainfile(filepath=os.path.splitext(out)[0] + ".blend")
    bpy.ops.export_scene.fbx(
        filepath=out,
        use_selection=False,
        object_types={'ARMATURE', 'MESH'},
        apply_scale_options='FBX_SCALE_ALL',
        apply_unit_scale=True,
        axis_forward='-Z',
        axis_up='Y',
        bake_space_transform=animated,  # identity armatures export as before
        add_leaf_bones=False,
        primary_bone_axis='Y',
        secondary_bone_axis='X',
        use_armature_deform_only=False,
        use_mesh_modifiers=False,  # keeps the shape keys
        mesh_smooth_type='FACE',
        # Every action its own take (one Unity clip each). The importer's NLA
        # tracks are muted copies of the same actions, so they are not exported.
        bake_anim=True,
        bake_anim_use_all_actions=True,
        bake_anim_use_nla_strips=False,
        bake_anim_force_startend_keying=True,
        bake_anim_simplify_factor=0.0,
        path_mode='COPY',
        embed_textures=True,
    )
    print("CASTLEMIST_VRCHAT_OK", out)


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    mode = MODE
    if "--mode" in args:
        i = args.index("--mode")
        if i + 1 >= len(args):
            raise SystemExit("--mode needs a value: character or model")
        mode = args[i + 1]
        del args[i:i + 2]
    src = args[0] if len(args) > 0 else GLB
    out = args[1] if len(args) > 1 else (FBX or os.path.splitext(src)[0] + ".fbx")
    if not src:
        raise SystemExit("usage: blender -b -P castlemist_vrchat.py -- <in.glb> [out.fbx] [--mode model|character]")
    convert(src, out, mode)
