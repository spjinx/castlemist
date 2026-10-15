/// @file
/// @brief The NPC catalogue: one record per character object, joined with the content map, plus a JSON cache.
/// @ingroup npc

#pragma once

#include "castlemist/format/content_map.h"
#include "castlemist/native/gw2dat.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace castlemist::npc {

/// A weapon an NPC carries: the character's slot field, the item dataId, the item's skin and the skin's model fileId.
struct Weapon {
    uint32_t slot = 0, item = 0, skin = 0, model = 0;
};

/// One character (cntc type 67) of the datastore.
struct NpcRecord {
    uint32_t id = 0;                  ///< Object dataId.
    std::string codename;             ///< Content slug.
    uint32_t model = 0;               ///< First MODL fileId of the content map entry, 0 if none.
    uint32_t sound_script = 0;        ///< First AMSP fileId of the content map entry, 0 if none.
    std::string race, modifier;       ///< Race token; "undead"/"ghost"/"".
    std::vector<Weapon> weapons;
    std::vector<uint32_t> skills;     ///< Skill dataIds.
    uint32_t minipet = 0;             ///< dataId of a minipet (type 48) referencing this character; lowest if several; 0 if none.
    std::string name;                 ///< Display name from the public API; empty until fetched.
};

/// Counts of how many records carry each kind of data.
struct Coverage {
    size_t npcs = 0, with_model = 0, with_race = 0, with_weapons = 0, with_skills = 0;
};

struct Catalogue {
    std::vector<NpcRecord> npcs;          ///< Sorted by id.
    std::vector<std::string> warnings;
    Coverage coverage() const;
    const NpcRecord* find(uint32_t id) const;
    std::vector<const NpcRecord*> by_model(uint32_t model) const;
};

/// Index-DB container of a fileId ("MODL", "AMSP", ...); "" when unknown.
using ContainerOf = std::function<std::string(uint32_t file_id)>;

/// Builds the catalogue from already-decompressed cntc packs. The content map must be built or loaded
/// (cmap::built()); otherwise the result is empty with the warning "content map not loaded".
Catalogue build_catalogue(const std::vector<castlemist::cmap::PackBytes>& packs, const ContainerOf& container_of);

/// As above, decompressing each cntc entry of `dat_path` first. `file_ids[i]` is entry i's fileId.
/// `progress(done, total)` is called after each entry when non-null.
Catalogue build_catalogue(const std::string& dat_path, const std::vector<MftData>& entries,
                          const std::vector<uint32_t>& file_ids, const ContainerOf& container_of,
                          const std::function<void(size_t, size_t)>& progress);

/// Sorted unique non-zero minipet dataIds of the catalogue.
std::vector<uint32_t> minipet_ids(const Catalogue&);

/// Sets `name` on every NPC whose `minipet` is a key of `minipet_names`; returns how many were set.
size_t apply_names(Catalogue&, const std::map<uint32_t, std::string>& minipet_names);

/// Writes the catalogue as JSON ("version": 2). False on I/O failure.
bool save(const Catalogue&, const std::filesystem::path&);

/// Reads a catalogue; nullopt for a missing file, bad JSON or any "version" other than 2. Never throws.
std::optional<Catalogue> load(const std::filesystem::path&);

}  // namespace castlemist::npc
