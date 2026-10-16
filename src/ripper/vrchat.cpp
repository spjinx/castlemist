#include "castlemist/ripper/vrchat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

#include "castlemist/character/key_store.h"
#include "castlemist/exportgltf/vrchat_export.h"
#include "castlemist/exportgltf/world_manifest.h"
#include "castlemist/ripper/face_morphs.h"
#include "castlemist/ripper/skeleton_merge.h"

// A private copy (see gw2dat_cli's note: several layers carry stb).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

bool blender_folder_newer(const std::string& a, const std::string& b) {
    // The numbers in each name, compared as numbers ("Blender 4.10" > "Blender 4.5").
    auto nums = [](const std::string& s) {
        std::vector<long> v;
        for (size_t i = 0; i < s.size();) {
            if (s[i] >= '0' && s[i] <= '9') {
                long n = 0;
                for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) n = n * 10 + (s[i] - '0');
                v.push_back(n);
            } else {
                ++i;
            }
        }
        return v;
    };
    const std::vector<long> va = nums(a), vb = nums(b);
    if (va != vb) return std::lexicographical_compare(vb.begin(), vb.end(), va.begin(), va.end());
    return a > b;
}

std::string find_blender() {
    fs::path best;
    std::error_code ec;
    for (const char* root : {"C:\\Program Files\\Blender Foundation", "C:\\Program Files (x86)\\Blender Foundation"}) {
        if (!fs::exists(root, ec)) continue;
        for (const auto& d : fs::directory_iterator(root, ec)) {
            const fs::path exe = d.path() / "blender.exe";
            if (fs::exists(exe, ec) &&
                (best.empty() ||
                 blender_folder_newer(d.path().filename().string(), best.parent_path().filename().string())))
                best = exe;
        }
    }
    return best.empty() ? std::string() : to_utf8(best);
}

namespace {

// One argument for CreateProcessW's command line, quoted as the MSVC runtime
// (and Blender's argv) reads it: backslashes before a quote are doubled.
std::wstring quote_arg(const std::wstring& a) {
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}

// Runs `<blender> -b --factory-startup --python-exit-code 1 -P <script> -- <args...>` headless;
// true when it exits with 0 inside `timeoutMs`, else false + `log`. With
// `logPath`, Blender's stdout and stderr go to that file, and on failure
// castlemist's reason is appended after them.
bool run_blender(const std::string& blender, const std::string& script, const std::vector<std::string>& args,
                 std::string& log, const std::string& logPath, unsigned long timeoutMs,
                 const std::atomic<bool>* cancel = nullptr) {
    // --python-exit-code: an uncaught exception in the script exits non-zero
    // (Blender's default is 0).
    std::wstring cmd = quote_arg(from_utf8(blender).wstring()) + L" -b --factory-startup --python-exit-code 1 -P " +
                       quote_arg(from_utf8(script).wstring()) + L" --";
    for (const std::string& a : args) cmd += L" " + quote_arg(from_utf8(a).wstring());

    HANDLE out = INVALID_HANDLE_VALUE;
    if (!logPath.empty()) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        out = CreateFileW(from_utf8(logPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    const bool capture = out != INVALID_HANDLE_VALUE;
    // The reason goes after whatever Blender wrote.
    auto fail = [&](std::string why) {
        log = std::move(why);
        if (capture) {
            const std::string line = "\r\n[castlemist] " + log + "\r\n";
            DWORD written = 0;
            SetFilePointer(out, 0, nullptr, FILE_END);
            WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            CloseHandle(out);
        }
        return false;
    };

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (capture) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = out;
        si.hStdError = out;
    }
    // Blender runs in a job object that kills it when the job's last handle
    // closes: if castlemist exits (or dies) during a conversion, Blender goes
    // with it instead of converting on, invisible, for up to the limit (review I5).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, capture ? TRUE : FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        if (job) CloseHandle(job);
        return fail("could not start Blender (" + std::to_string(GetLastError()) + ")");
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);   // still runs, only without the kill-on-exit guarantee
        job = nullptr;
    }
    ResumeThread(pi.hThread);
    // Waits in slices so a cancel (the app closing) stops Blender promptly.
    const ULONGLONG start = GetTickCount64();
    DWORD w = WAIT_TIMEOUT;
    bool cancelled = false;
    for (;;) {
        const ULONGLONG used = GetTickCount64() - start;
        if (used >= timeoutMs) break;
        w = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(std::min<ULONGLONG>(250, timeoutMs - used)));
        if (w != WAIT_TIMEOUT) break;
        if (cancel && cancel->load()) { cancelled = true; break; }
    }
    if (w != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10 * 1000);   // let it go before the log is appended to
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (cancelled) return fail("Blender stopped: export cancelled");
    if (w != WAIT_OBJECT_0) return fail("Blender timed out after " + std::to_string(timeoutMs / 1000) + " s");
    if (code != 0) return fail("Blender exited with " + std::to_string(code));
    if (capture) CloseHandle(out);
    return true;
}

// The model / avatar conversion: <glb> <fbx> [--mode <mode>]; it must also leave the .fbx.
bool run_blender(const std::string& blender, const std::string& script, const std::string& glb, const std::string& fbx,
                 std::string& log, const char* mode = nullptr, const std::string& logPath = {}) {
    std::vector<std::string> args = {glb, fbx};
    if (mode) {
        args.push_back("--mode");
        args.push_back(mode);
    }
    std::string why;
    const bool ran = run_blender(blender, script, args, why, logPath, 10 * 60 * 1000);
    std::error_code ec;
    if (ran && fs::exists(from_utf8(fbx), ec)) return true;
    if (!ran) {   // the reason is already in the log
        log = why + " and no .fbx";
        return false;
    }
    log = "Blender exited with 0 and no .fbx";
    if (!logPath.empty()) std::ofstream(from_utf8(logPath), std::ios::binary | std::ios::app) << "\r\n[castlemist] " << log << "\r\n";
    return false;
}

// tools/blender/<name> in the castlemist tree the running executable sits in.
std::string default_script(const char* name = "castlemist_vrchat.py") {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return to_utf8(character::find_castlemist_root(fs::path(exe).parent_path()) / "tools" / "blender" / name);
}

std::string safe_name(std::string s) {
    for (char& c : s)
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    return s.empty() ? "avatar" : s;
}

} // namespace

std::string write_vrchat_maps(const ModelPreview& model, const std::string& glb_path) {
    const fs::path glb = from_utf8(glb_path);
    const fs::path dir = glb.parent_path() / (glb.stem().wstring() + L" Textures");
    std::error_code ec;
    bool any = false;
    auto save = [&](const std::string& file, int w, int h, const std::vector<uint8_t>& rgba) {
        if (w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return;
        fs::create_directories(dir, ec);
        const fs::path path = dir / from_utf8(file + ".png");
        int len = 0;
        unsigned char* png = stbi_write_png_to_mem(rgba.data(), w * 4, w, h, 4, &len);
        if (!png) return;
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png), len);
        STBIW_FREE(png);
        any = true;
    };
    auto tex = [&](int i) -> const ModelTextureCPU* {
        return i >= 0 && static_cast<size_t>(i) < model.textures.size() ? &model.textures[static_cast<size_t>(i)] : nullptr;
    };
    for (const ModelMaterialCPU& mat : model.materials) {
        if (mat.materialName != "CharacterAtlas" && mat.materialName != "BodyAtlas") continue;
        const std::string n = mat.materialName;
        if (const ModelTextureCPU* t = tex(mat.diffuseTex)) save(n + " - BaseColor", t->width, t->height, t->rgba);
        if (const ModelTextureCPU* t = tex(mat.normalTex)) save(n + " - Normal", t->width, t->height, t->rgba);
        if (const ModelTextureCPU* t = tex(mat.emissiveTex)) save(n + " - Emission", t->width, t->height, t->rgba);
        if (const ModelTextureCPU* t = tex(mat.metalRoughTex)) {
            // glTF (G = roughness, B = metal) -> Unity (R = metal, A = smoothness).
            std::vector<uint8_t> ms(t->rgba.size());
            for (size_t i = 0; i + 3 < ms.size(); i += 4) {
                ms[i] = ms[i + 1] = ms[i + 2] = t->rgba[i + 2];
                ms[i + 3] = static_cast<uint8_t>(255 - t->rgba[i + 1]);
            }
            save(n + " - MetallicSmoothness", t->width, t->height, ms);
        }
        for (const auto& ex : mat.extraTextures)
            if (ex.role == "decal")
                if (const ModelTextureCPU* t = tex(ex.texIndex)) save(n + " - Detail", t->width, t->height, t->rgba);
    }
    return any ? to_utf8(dir) : std::string();
}

VrchatModelReport export_vrchat_model(const ModelPreview& model, const std::string& parentDirUtf8,
                                      const std::string& name, uint32_t modelFileId, const VrchatOptions& vrc) {
    VrchatModelReport r;
    const std::string safe = exportgltf::safe_file_name(name);
    const fs::path dir = from_utf8(parentDirUtf8) / from_utf8(safe);
    const exportgltf::VrchatFolderResult f = exportgltf::write_vrchat_folder(model, to_utf8(dir), safe, modelFileId);
    r.folder = f.folder.empty() ? to_utf8(dir) : f.folder;
    r.glb = f.glb;
    r.materials = f.materials;
    r.clips = f.clips;
    r.warnings = f.warnings;
    if (!f.ok) {
        r.error = f.error;
        return r;
    }
    r.ok = true;
    if (vrc.blender_exe == "-") {
        r.blender = "Blender skipped (disabled)";
        return r;
    }
    const std::string blender = vrc.blender_exe.empty() ? find_blender() : vrc.blender_exe;
    const std::string script = vrc.script.empty() ? default_script() : vrc.script;
    std::error_code ec;
    if (blender.empty()) {
        r.blender = "Blender not found -- open the .glb in Blender and run tools/blender/castlemist_vrchat.py --mode model";
    } else if (!fs::exists(from_utf8(script), ec)) {
        r.blender = "conversion script missing: " + script;
    } else {
        const std::string fbx = to_utf8(dir / from_utf8(safe + ".fbx"));
        // Blender's own output lands in the log; a stale one from an earlier run never survives.
        const fs::path logPath = dir / from_utf8(safe + " blender.log");
        fs::remove(logPath, ec);
        std::string log;
        if (run_blender(blender, script, r.glb, fbx, log, "model", to_utf8(logPath))) {
            r.fbx = fbx;
            r.blender = blender;
            fs::remove(logPath, ec);
        } else {
            r.blender = log;
            if (!fs::exists(logPath, ec))  // the log could not be opened for Blender
                std::ofstream(logPath, std::ios::binary) << log << '\n';
            r.blenderLog = to_utf8(logPath);
        }
    }
    return r;
}

bool convert_world_to_fbx(const std::string& exportDirUtf8, std::string& log, const std::string& blenderExe,
                          size_t* fbxSet, const std::atomic<bool>* cancel) {
    if (fbxSet) *fbxSet = 0;
    const std::string blender = blenderExe.empty() ? find_blender() : blenderExe;
    if (blender.empty()) {
        log = "Blender not found";
        return false;
    }
    std::error_code ec;
    const std::string script = default_script("castlemist_world.py");
    if (!fs::exists(from_utf8(script), ec)) {
        log = "conversion script missing: " + script;
        return false;
    }
    const fs::path dir = from_utf8(exportDirUtf8);
    if (!fs::is_regular_file(dir / "manifest.json", ec)) {
        log = "no manifest.json in " + exportDirUtf8;
        return false;
    }
    const std::string logPath = to_utf8(dir / "blender.log");
    std::string why;
    // One session for the whole folder (spec D6); Spirit Vale's time is the
    // plan's measurement, so the limit is generous.
    const bool ran = run_blender(blender, script, {"--to-fbx", exportDirUtf8}, why, logPath, 4ul * 60 * 60 * 1000, cancel);
    // Whatever was converted is recorded, also after a failure part-way.
    const size_t n = exportgltf::set_manifest_fbx(exportDirUtf8);
    if (fbxSet) *fbxSet = n;
    if (!ran) {
        log = why + " (log: " + logPath + "); " + std::to_string(n) + " .fbx recorded in the manifest";
        return false;
    }
    log = "Blender " + blender + ": " + std::to_string(n) + " .fbx recorded in the manifest (log: " + logPath + ")";
    return true;
}

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
    if (script.empty()) script = default_script();
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
      << "     Every map is also in the \"" << name << " Textures\" folder: BaseColor, Normal, Emission,\n"
      << "     MetallicSmoothness (Unity layout: R = metal, A = smoothness) and Detail (a set's highlight\n"
      << "     layer, if any). Armor alpha is already a clean cut-out; the shine it carried is in\n"
      << "     MetallicSmoothness. How each armor piece is made:\n"
      ;
    for (const AssemblyPart& p : r.assembly.parts)
        if (!p.reason.empty() && p.reason.find('(') != std::string::npos && p.name.rfind("Weapon", 0) != 0)
            n << "       " << p.name << ": " << p.reason << "\n";
    n << "  3. Drag the model into the scene, add a VRC Avatar Descriptor:\n"
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
      << "     hands = (3,0), feet = (0,1), undergarments = (1,1).\n"
      << "     The Undergarments mesh is the race's underwear (worn wherever no armor covers it in game);\n"
      << "     it stays when the body tiles are discarded -- toggle or discard it with the armor if it pokes through.\n"
      << "     Body, Hair and Undergarments use BodyAtlas (their textures before any armor is painted over them);\n"
      << "     the armor uses CharacterAtlas.\n"
      << "  6. Face details: the Body mesh carries <slider>+ / <slider>- blend shapes (Cheeks+, Jaw Width-, ...)\n"
      << "     already set to the saved look; adjust them in the SkinnedMeshRenderer if you like.\n"
      << "\nBones and blend shapes are generated from the game's own rig; visemes and blink are\n"
      << "approximations (GW2 has none) -- tweak in Blender if a mouth shape looks off.\n";
    r.ok = true;
    return r;
}

} // namespace castlemist::ripper
