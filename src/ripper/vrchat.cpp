#include "castlemist/ripper/vrchat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

#include "castlemist/character/key_store.h"
#include "castlemist/ripper/face_morphs.h"
#include "castlemist/ripper/skeleton_merge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace castlemist::ripper {
namespace fs = std::filesystem;

namespace {

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string to_utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Meshes of one avatar piece (meshName: Body, Hair, Coat, ...) sharing a
// material and skinning become one: vertices appended, indices offset, blend
// shapes unioned by name (zero where a part has none). Pieces stay apart.
void merge_meshes_by_piece(ModelPreview& model) {
    std::map<std::tuple<std::string, uint32_t, bool>, std::vector<size_t>> groups;
    for (size_t i = 0; i < model.meshes.size(); ++i)
        groups[{model.meshes[i].meshName, model.meshes[i].materialIndex, model.meshes[i].hasSkin}].push_back(i);
    std::vector<ModelMeshCPU> merged;
    for (const auto& [key, idx] : groups) {
        if (idx.size() == 1) {
            merged.push_back(std::move(model.meshes[idx[0]]));
            continue;
        }
        ModelMeshCPU out;
        out.meshName = std::get<0>(key);
        out.materialIndex = std::get<1>(key);
        out.hasSkin = std::get<2>(key);
        out.hasTangents = true;
        std::vector<std::string> names;  // union of blend shapes, first-seen order
        for (size_t i : idx)
            for (const MorphTargetCPU& t : model.meshes[i].morphs)
                if (std::find(names.begin(), names.end(), t.name) == names.end()) names.push_back(t.name);
        size_t total = 0;
        for (size_t i : idx) total += model.meshes[i].vertices.size();
        out.morphs.resize(names.size());
        for (size_t k = 0; k < names.size(); ++k) {
            out.morphs[k].name = names[k];
            out.morphs[k].delta.assign(total * 3, 0.0f);
        }
        size_t base = 0;
        for (size_t i : idx) {
            ModelMeshCPU& m = model.meshes[i];
            out.hasTangents = out.hasTangents && m.hasTangents;
            out.exportUv1 = out.exportUv1 || m.exportUv1;
            for (uint32_t ix : m.indices) out.indices.push_back(static_cast<uint32_t>(base + ix));
            for (const MorphTargetCPU& t : m.morphs) {
                const size_t k = static_cast<size_t>(std::find(names.begin(), names.end(), t.name) - names.begin());
                out.morphs[k].weight = std::max(out.morphs[k].weight, t.weight);
                std::copy(t.delta.begin(), t.delta.end(), out.morphs[k].delta.begin() + static_cast<std::ptrdiff_t>(base * 3));
            }
            out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
            base += m.vertices.size();
        }
        out.vertexCount = static_cast<uint32_t>(out.vertices.size());
        merged.push_back(std::move(out));
    }
    model.meshes = std::move(merged);
}

bool is_dynamic_name(const std::string& n) {
    for (const char* k : {"Hair", "Cloth", "Stem", "Cape", "Tail", "Skirt", "Ribbon", "Tassel"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}

} // namespace

std::string humanoid_name(const std::string& j) {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m = {
            // VRChat: the shoulders and neck must be direct children of the Chest,
            // and GW2 hangs them on Spine03 -- so Spine03 is the Chest and Spine02
            // folds into Spine (fold_joints).
            {"bone:COG", "Hips"}, {"bone:Spine01", "Spine"}, {"bone:Spine03", "Chest"},
            // bone:Jaw stays unmapped ("JawBone"): lip sync runs on the vrc.v_* blend
            // shapes, and a humanoid Jaw would fight them.
            {"bone:Neck01", "Neck"}, {"bone:Head", "Head"}, {"bone:Jaw", "JawBone"},
        };
        for (const char* side : {"L", "R"}) {
            const std::string s = side, u = s == "L" ? "Left" : "Right";
            m["bone:Clavicle" + s] = u + "Shoulder";
            m["bone:Shoulder" + s] = u + "UpperArm";
            m["bone:Elbow" + s] = u + "LowerArm";
            m["bone:Wrist" + s] = u + "Hand";
            m["bone:Hip" + s] = u + "UpperLeg";
            m["bone:Knee" + s] = u + "LowerLeg";
            m["bone:Ankle" + s] = u + "Foot";
            m["bone:Ball" + s] = u + "Toes";
            m["bone:Eye" + s] = u + "Eye";
            for (const auto& [gw2, unity] : std::vector<std::pair<std::string, std::string>>{
                     {"Thumb", "Thumb"}, {"Index", "Index"}, {"Middle", "Middle"}, {"Ring", "Ring"}, {"Pinky", "Little"}}) {
                m["bone:" + gw2 + s + "01"] = u + unity + "Proximal";
                m["bone:" + gw2 + s + "02"] = u + unity + "Intermediate";
                m["bone:" + gw2 + s + "03"] = u + unity + "Distal";
            }
        }
        return m;
    }();
    auto it = map.find(j);
    return it == map.end() ? std::string() : it->second;
}

namespace {

int joint_index(const ModelPreview& m, const std::string& name) {
    for (size_t i = 0; i < m.joints.size(); ++i)
        if (m.joints[i].name == name) return static_cast<int>(i);
    return -1;
}

// Re-derives a joint's bind local transform after its parent changed.
void relocal(ModelPreview& m, size_t ji) {
    const std::array<float, 7> l = local_from_bind(m, ji);
    ModelJoint& j = m.joints[ji];
    for (int k = 0; k < 3; ++k) j.localPos[k] = l[static_cast<size_t>(k)];
    for (int k = 0; k < 4; ++k) j.localQuat[k] = l[static_cast<size_t>(3 + k)];
    for (int k = 0; k < 9; ++k) j.localScale[k] = (k % 4 == 0) ? 1.0f : 0.0f;
}

// Removes joint `name`: its skin weight goes to its parent, its children hang
// from that parent (bind pose unchanged). What CATS' "Fix Model" does for the
// extra spine, pelvis and twist bones VRChat's IK trips over.
void fold_joint(ModelPreview& m, const std::string& name) {
    const int ji = joint_index(m, name);
    if (ji < 0) return;
    const int parent = m.joints[static_cast<size_t>(ji)].parent;
    if (parent < 0) return;
    for (ModelMeshCPU& mesh : m.meshes)
        for (GVertex& v : mesh.vertices) {
            for (int k = 0; k < 4; ++k)
                if (v.bidx[k] == static_cast<uint32_t>(ji)) v.bidx[k] = static_cast<uint32_t>(parent);
            for (int a = 0; a < 4; ++a)  // one bone, one slot
                for (int b = a + 1; b < 4; ++b)
                    if (v.bwt[b] > 0 && v.bidx[a] == v.bidx[b]) {
                        v.bwt[a] += v.bwt[b];
                        v.bwt[b] = 0;
                    }
        }
    for (size_t c = 0; c < m.joints.size(); ++c)
        if (m.joints[c].parent == ji) {
            m.joints[c].parent = parent;
            relocal(m, c);
        }
    // Erase it; shift every index past it.
    m.joints.erase(m.joints.begin() + ji);
    for (ModelJoint& j : m.joints)
        if (j.parent > ji) --j.parent;
    for (ModelMeshCPU& mesh : m.meshes)
        for (GVertex& v : mesh.vertices)
            for (int k = 0; k < 4; ++k)
                if (v.bidx[k] > static_cast<uint32_t>(ji)) --v.bidx[k];
}

// The humanoid chain VRChat wants: Hips > Spine > Chest > (Neck, Shoulders),
// Hips > UpperLeg, eyes straight under the Head, no twist bones in the arms.
void fix_hierarchy(ModelPreview& m) {
    for (const char* j : {"bone:Spine02", "bone:Pelvis", "bone:TwistElbowL", "bone:TwistElbowR", "bone:TwistWristL",
                          "bone:TwistWristR"})
        fold_joint(m, j);
    const int head = joint_index(m, "bone:Head");
    for (const char* eye : {"bone:EyeL", "bone:EyeR"}) {
        const int e = joint_index(m, eye);
        if (e >= 0 && head >= 0 && m.joints[static_cast<size_t>(e)].parent != head) {
            m.joints[static_cast<size_t>(e)].parent = head;
            relocal(m, static_cast<size_t>(e));
        }
    }
}

// Drops joints nothing needs: not humanoid, no skin weight, no kept child.
void prune_joints(ModelPreview& model) {
    const size_t n = model.joints.size();
    std::vector<bool> keep(n, false);
    for (const ModelMeshCPU& m : model.meshes)
        for (const GVertex& v : m.vertices)
            for (int k = 0; k < 4; ++k)
                if (v.bwt[k] > 0 && v.bidx[k] < n) keep[v.bidx[k]] = true;
    for (size_t i = 0; i < n; ++i)
        if (!humanoid_name(model.joints[i].name).empty()) keep[i] = true;
    for (size_t i = 0; i < n; ++i)  // ancestors of anything kept
        if (keep[i])
            for (int p = model.joints[i].parent; p >= 0 && static_cast<size_t>(p) < n && !keep[static_cast<size_t>(p)];
                 p = model.joints[static_cast<size_t>(p)].parent)
                keep[static_cast<size_t>(p)] = true;
    std::vector<int> remap(n, -1);
    std::vector<ModelJoint> kept;
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) {
            remap[i] = static_cast<int>(kept.size());
            kept.push_back(model.joints[i]);
        }
    for (ModelJoint& j : kept)
        j.parent = j.parent >= 0 && static_cast<size_t>(j.parent) < n ? remap[static_cast<size_t>(j.parent)] : -1;
    for (ModelMeshCPU& m : model.meshes)
        for (GVertex& v : m.vertices)
            for (int k = 0; k < 4; ++k)
                v.bidx[k] = v.bidx[k] < n && remap[v.bidx[k]] >= 0 ? static_cast<uint32_t>(remap[v.bidx[k]]) : 0;
    model.joints = std::move(kept);
}

} // namespace

std::vector<std::string> make_vrchat_ready(ModelPreview& model) {
    // The same two UV sets on every mesh -- UV0, and UV1 (tile 0 unless a body
    // part already has its discard tile) -- so joined meshes keep them aligned.
    for (ModelMeshCPU& m : model.meshes) {
        if (m.exportUv1) continue;
        m.exportUv1 = true;
        for (GVertex& v : m.vertices) {
            v.uv1[0][0] = std::clamp(v.u - std::floor(v.u), 1e-4f, 1 - 1e-4f);
            v.uv1[0][1] = std::clamp(v.v - std::floor(v.v), 1e-4f, 1 - 1e-4f);
        }
    }
    add_vrchat_face_keys(model);
    merge_meshes_by_piece(model);
    fix_hierarchy(model);
    prune_joints(model);
    for (ModelMaterialCPU& m : model.materials) {  // "import2:AmatShader1" -> "AmatShader1"
        const size_t c = m.materialName.rfind(':');
        if (c != std::string::npos) m.materialName = m.materialName.substr(c + 1);
    }

    // Rename: humanoid names first, then the rest without prefixes, unique.
    std::set<std::string> used;
    std::vector<std::string> renamed(model.joints.size());
    for (size_t i = 0; i < model.joints.size(); ++i) {
        renamed[i] = humanoid_name(model.joints[i].name);
        if (!renamed[i].empty()) used.insert(renamed[i]);
    }
    for (size_t i = 0; i < model.joints.size(); ++i) {
        if (!renamed[i].empty()) continue;
        std::string n = model.joints[i].name;
        if (n.rfind("actionpoint:", 0) == 0) n = "AP_" + n.substr(12);
        else if (n.rfind("bone:", 0) == 0) n = n.substr(5);
        std::replace(n.begin(), n.end(), ':', '_');
        if (n.empty()) n = "Joint";
        std::string unique = n;
        for (int k = 2; used.count(unique); ++k) unique = n + "_" + std::to_string(k);
        used.insert(unique);
        renamed[i] = unique;
    }
    // PhysBone chain roots: dynamic bones whose parent isn't one, and a back
    // item's own bones (hung under a holster point).
    std::vector<std::string> chains;
    for (size_t i = 0; i < model.joints.size(); ++i) {
        const int p = model.joints[i].parent;
        const bool has_parent = p >= 0 && static_cast<size_t>(p) < model.joints.size();
        const bool under_holster = has_parent &&
                                   model.joints[static_cast<size_t>(p)].name.rfind("actionpoint:", 0) == 0 &&
                                   model.joints[i].name.rfind("actionpoint:", 0) != 0;
        if (under_holster) {
            chains.push_back(renamed[i]);
            continue;
        }
        if (!is_dynamic_name(renamed[i])) continue;
        if (has_parent && is_dynamic_name(renamed[static_cast<size_t>(p)])) continue;
        chains.push_back(renamed[i]);
    }
    for (size_t i = 0; i < model.joints.size(); ++i) model.joints[i].name = renamed[i];
    return chains;
}

std::string find_blender() {
    fs::path best;
    std::error_code ec;
    for (const char* root : {"C:\\Program Files\\Blender Foundation", "C:\\Program Files (x86)\\Blender Foundation"}) {
        if (!fs::exists(root, ec)) continue;
        for (const auto& d : fs::directory_iterator(root, ec)) {
            const fs::path exe = d.path() / "blender.exe";
            if (fs::exists(exe, ec) && (best.empty() || d.path().filename().string() > best.parent_path().filename().string()))
                best = exe;
        }
    }
    return best.empty() ? std::string() : to_utf8(best);
}

namespace {

// Runs Blender headless on the conversion script; false + `log` on failure.
bool run_blender(const std::string& blender, const std::string& script, const std::string& glb, const std::string& fbx,
                 std::string& log) {
    std::wstring cmd = L"\"" + from_utf8(blender).wstring() + L"\" -b --factory-startup -P \"" +
                       from_utf8(script).wstring() + L"\" -- \"" + from_utf8(glb).wstring() + L"\" \"" +
                       from_utf8(fbx).wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        log = "could not start Blender (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    const DWORD w = WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::error_code ec;
    if (w != WAIT_OBJECT_0) {
        log = "Blender timed out";
        return false;
    }
    if (code != 0 || !fs::exists(from_utf8(fbx), ec)) {
        log = "Blender exited with " + std::to_string(code) + " and no .fbx";
        return false;
    }
    return true;
}

std::string safe_name(std::string s) {
    for (char& c : s)
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    return s.empty() ? "avatar" : s;
}

} // namespace

VrchatReport export_vrchat(const character::CharacterManifest& manifest, const std::string& dat_path,
                           const std::string& out_dir, AssemblyOptions options, const VrchatOptions& vrc) {
    VrchatReport r;
    std::error_code ec;
    const fs::path dir = from_utf8(out_dir);
    fs::create_directories(dir, ec);
    const std::string name = safe_name(manifest.name);
    r.glb = to_utf8(dir / from_utf8(name + ".glb"));
    options.metres = true;
    options.vrchat = true;
    r.assembly = assemble_character(manifest, dat_path, r.glb, options);
    if (!r.assembly.ok) {
        r.error = r.assembly.error;
        return r;
    }
    r.physbone_chains = r.assembly.physbone_chains;
    r.joints = r.assembly.joints;

    const std::string blender = vrc.blender_exe.empty() ? find_blender() : vrc.blender_exe;
    std::string script = vrc.script;
    if (script.empty()) {
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        script = to_utf8(character::find_castlemist_root(fs::path(exe).parent_path()) / "tools" / "blender" /
                         "castlemist_vrchat.py");
    }
    if (blender.empty()) {
        r.blender = "Blender not found -- open the .glb in Blender and run tools/blender/castlemist_vrchat.py";
    } else if (!fs::exists(from_utf8(script), ec)) {
        r.blender = "conversion script missing: " + script;
    } else {
        const std::string fbx = to_utf8(dir / from_utf8(name + ".fbx"));
        std::string log;
        if (run_blender(blender, script, r.glb, fbx, log)) {
            r.fbx = fbx;
            r.blender = blender;
        } else {
            r.blender = log;
        }
    }

    // The setup note: what to do in Unity, and the chains to give PhysBones.
    r.notes = to_utf8(dir / from_utf8(name + " - VRChat setup.txt"));
    std::ofstream n(from_utf8(r.notes), std::ios::binary);
    n << manifest.name << " -- VRChat avatar from castlemist\n\n"
      << "Files\n"
      << "  " << name << (r.fbx.empty() ? ".glb  (no Blender found: run tools/blender/castlemist_vrchat.py on it)\n"
                                         : ".fbx  (import this into Unity; the .glb is the same avatar for Blender)\n")
      << "\nUnity (VRChat Creator Companion project, Avatars SDK)\n"
      << "  1. Drag the .fbx into Assets. Select it > Rig > Animation Type: Humanoid > Apply.\n"
      << "     The bones are named for Unity's auto-mapper (Hips, Spine, Chest, Neck, Head,\n"
      << "     LeftShoulder / LeftUpperArm / LeftLowerArm / LeftHand, LeftUpperLeg / LeftLowerLeg / LeftFoot /\n"
      << "     LeftToes, fingers, LeftEye / RightEye, Jaw). Configure... should show every bone green.\n"
      << "  2. Materials tab > Extract Textures, then give the materials Poiyomi (or VRChat/Mobile/Toon Lit).\n"
      << "     CharacterAtlas: base colour + normal map, alpha cutout 0.25; its emissive map is the glow.\n"
      << "  3. Drag the model into the scene, add a VRC Avatar Descriptor:\n"
      << "     View Position: between the eyes.  LipSync: Viseme Blend Shape, mesh Body -- the vrc.v_* keys\n"
      << "     are already named so 'Auto Detect' fills them.  Eye Look: eyes LeftEye / RightEye,\n"
      << "     Eyelids: Blendshapes, Blink = Blink.\n"
      << "  4. Add a VRC Phys Bone on each chain root below (hair, cloth, stems):\n";
    for (const std::string& c : r.physbone_chains) n << "       " << c << "\n";
    if (r.physbone_chains.empty()) n << "       (none found)\n";
    n << "  5. Each armor piece is its own mesh (Coat, Gloves, Boots, Helm, ...) -- toggle them with\n"
      << "     animations. The whole body is kept; hide skin under armor with Poiyomi's UV Tile Discard on\n"
      << "     the BodyAtlas material, UV channel UVDiscard (UV1). Tiles count from the bottom-left,\n"
      << "     (0,0) (1,0) (2,0) (3,0) then the row above: head = (0,0), chest = (1,0), legs = (2,0),\n"
      << "     hands = (3,0), feet = (0,1).\n"
      << "     Body and Hair use BodyAtlas (their textures before any armor is painted over them);\n"
      << "     the armor uses CharacterAtlas.\n"
      << "  6. Face details: the Body mesh carries <slider>+ / <slider>- blend shapes (Cheeks+, Jaw Width-, ...)\n"
      << "     already set to the saved look; adjust them in the SkinnedMeshRenderer if you like.\n"
      << "\nBones and blend shapes are generated from the game's own rig; visemes and blink are\n"
      << "approximations (GW2 has none) -- tweak in Blender if a mouth shape looks off.\n";
    r.ok = true;
    return r;
}

} // namespace castlemist::ripper
