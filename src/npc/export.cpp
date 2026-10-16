#include "castlemist/npc/export.h"

#include "castlemist/npc/look.h"
#include "castlemist/npc/tokens.h"

#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/extract/entry_extractor.h"
#include "castlemist/ripper/assemble.h"
#include "castlemist/ripper/character_export.h"

#include <exception>
#include <fstream>
#include <optional>

namespace castlemist::npc {

std::string export_folder_name(const NpcRecord& r) {
    std::string name = std::to_string(r.id);
    if (r.codename.empty()) return name;
    name += '_';
    for (unsigned char c : r.codename) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
        name += ok ? static_cast<char>(c) : '_';
    }
    return name;
}

nlohmann::json export_manifest(const NpcRecord& r, const std::vector<ClipInfo>& clips,
                               const std::vector<WrittenWeapon>& weapons,
                               const std::vector<std::string>& files, const std::vector<std::string>& notes,
                               const LookUsed* look) {
    using nlohmann::json;
    json j;
    j["version"] = 1;
    j["id"] = r.id;
    j["codename"] = r.codename;
    j["model"] = r.model;
    j["sound_script"] = r.sound_script;
    j["race"] = r.race;
    j["modifier"] = r.modifier;
    j["clips"] = json::array();
    for (const ClipInfo& c : clips)
        j["clips"].push_back({{"name", c.name},
                              {"group", clip_group(c.name)},
                              {"prefix", clip_prefix(c.name)},
                              {"duration", c.duration}});
    j["weapons"] = json::array();
    for (const WrittenWeapon& w : weapons)
        j["weapons"].push_back({{"slot", w.weapon.slot},
                                {"item", w.weapon.item},
                                {"skin", w.weapon.skin},
                                {"model", w.weapon.model},
                                {"file", w.file}});
    j["looks"] = r.looks.size();
    if (look) {
        j["look_used"] = look->index;
        j["parts"] = json::array();
        for (const PartInfo& p : look->parts)
            j["parts"].push_back({{"name", p.name}, {"status", p.status}, {"reason", p.reason}});
    }
    j["skills"] = r.skills;
    j["files"] = files;
    j["notes"] = notes;
    return j;
}

std::vector<std::string> base_notes(const NpcRecord& r) {
    std::vector<std::string> notes;
    if (r.model == 0)
        notes.push_back(r.race.empty() ? "no body model"
                                       : "no body model: race-body NPC (" + r.race + "), outfit data not decoded");
    for (const Weapon& w : r.weapons)
        if (w.model == 0) notes.push_back("weapon " + std::to_string(w.slot) + ": no skin model");
    return notes;
}

ExportResult export_npc(Gw2Dat& dat, const NpcRecord& r, const std::filesystem::path& out_dir) {
    return export_npc(dat, r, out_dir, ExportOptions{});
}

ExportResult export_npc(Gw2Dat& dat, const NpcRecord& r, const std::filesystem::path& out_dir,
                        const ExportOptions& options) {
    ExportResult res;
    res.notes = base_notes(r);
    std::vector<ClipInfo> clips;
    std::vector<WrittenWeapon> weapons;
    try {
        res.folder = out_dir / export_folder_name(r);
        std::filesystem::create_directories(res.folder);
    } catch (const std::exception& e) {
        res.notes.push_back(std::string("folder: ") + e.what());
        return res;
    }

    // Loads one model and writes it; returns true when the glb was written. Fills `clips` when asked.
    auto write_model = [&](uint32_t model, const std::string& file, std::vector<ClipInfo>* out_clips) {
        try {
            std::shared_ptr<ModelPreview> m = load_model_by_fileid(dat, model);
            if (!m) {
                res.notes.push_back(file + ": model failed to load");
                return false;
            }
            if (out_clips)
                for (const auto& c : m->animClips) out_clips->push_back({c.name, c.duration});
            auto er = exportgltf::export_model_gltf(*m, (res.folder / file).string());
            if (!er.ok) {
                res.notes.push_back(file + ": " + er.error);
                return false;
            }
            res.files.push_back(file);
            return true;
        } catch (const std::exception& e) {
            res.notes.push_back(file + ": " + e.what());
            return false;
        }
    };

    if (r.model != 0) write_model(r.model, "body.glb", &clips);

    // A race-body NPC: dress the Character Ripper's body from the look's composite variant.
    std::optional<LookUsed> look_used;
    if (r.model == 0 && !r.looks.empty()) {
        try {
            std::optional<composite::Composite> comp = ripper::load_composite(dat);
            std::string why;
            std::optional<NpcAssembly> as;
            if (comp)
                as = npc_assembly(r, options.look, *comp, &why);
            else
                why = "no Composite in the dat";
            if (!as) {
                res.notes.push_back("look " + std::to_string(options.look) + ": " + why);
            } else {
                // The ripper leaves aquatic pieces off the land body itself (assemble.cpp worn_slots), so none are dropped here.
                ripper::AssemblyReport rep = ripper::assemble_character(
                    as->manifest, dat.file_info.file_path, (res.folder / "body.glb").string(), as->options);
                for (const std::string& n : as->notes) res.notes.push_back(n);
                if (!rep.ok) {
                    res.notes.push_back("body.glb: " + rep.error);
                } else {
                    // The generic "no body model" note is stale now.
                    std::erase_if(res.notes, [](const std::string& n) { return n.rfind("no body model", 0) == 0; });
                    res.files.push_back("body.glb");
                    LookUsed lu;
                    lu.index = options.look;
                    for (const auto& p : rep.parts) lu.parts.push_back({p.name, p.status, p.reason});
                    look_used = std::move(lu);
                }
            }
        } catch (const std::exception& e) {
            res.notes.push_back(std::string("body.glb: ") + e.what());
        }
    }
    for (const Weapon& w : r.weapons) {
        if (w.model == 0) continue;
        std::string file = "weapon_" + std::to_string(w.slot) + ".glb";
        bool written = write_model(w.model, file, nullptr);
        weapons.push_back({w, written ? file : std::string()});
    }

    try {
        std::vector<std::string> files = res.files;
        files.push_back("manifest.json");
        nlohmann::json j = export_manifest(r, clips, weapons, files, res.notes, look_used ? &*look_used : nullptr);
        std::ofstream out(res.folder / "manifest.json", std::ios::binary);
        out << j.dump(2) << '\n';
        out.close();
        if (out) {
            res.files = files;
            res.ok = true;
        } else {
            res.notes.push_back("manifest.json: write failed");
        }
    } catch (const std::exception& e) {
        res.notes.push_back(std::string("manifest.json: ") + e.what());
    }
    return res;
}

}  // namespace castlemist::npc
