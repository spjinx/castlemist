#ifndef CONTENT_MAP_H
#define CONTENT_MAP_H

// Offline chat-link id -> dat asset map, built from the GW2 content datastore
// (cntc / PackContent packfiles). A chat link carries a game/API id (item id,
// skin id, ...); the .dat addresses assets by fileId. The content datastore
// bridges the two: each content object has a contentType (item = 35) at +16, its
// API / chat-link "dataId" at +40 (+20 is an internal uid, NOT the chat-link id),
// and its asset references in slots recorded by the pack's `fileIndices` fixup
// table. Each slot holds an index into the datastore's single shared fileRefs
// table (carried by one pack), which decodes to the real fileId. Verified against
// a live Gw2.dat + the GW2 API: item 76158 / skin 6506 (Astralaria) -> icon
// 1200325, skin model 1200313.
//
// build() parses every cntc pack once into an in-memory (contentType,id)->fileId
// table; resolve() answers lookups. The table can be cached to disk so it only
// has to be built once per game version.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "castlemist/native/gw2dat.h"

namespace castlemist::cmap {

/// GW2 Content::CONTENT_TYPE_* values (from the client, IDA-verified). The map
/// stores every type it sees; these are the ones the chat link decoder maps.
constexpr uint32_t CONTENT_TYPE_ITEM = 35;
constexpr uint32_t CONTENT_TYPE_OUTFIT = 51;
constexpr uint32_t CONTENT_TYPE_SKIN = 66;
constexpr uint32_t CONTENT_TYPE_CONTAINER = 240;   // a container item's contents list
constexpr uint32_t CONTENT_TYPE_MOUNT_SKIN = 302;  // dataId = /v2/mounts/skins id

/// The chat link header byte -> content type it resolves against (0 = unmapped).
uint32_t content_type_for_header(uint8_t header);

bool built();
size_t size();  // number of (type,id) entries currently held
void clear();

/// Parse the supplied cntc entries (raw MftData, copied by the caller so this is
/// safe on a background thread) and build the map. `dat_path` is the archive the
/// entries live in. `file_ids[i]` is entry i's fileId (any of them; 0 = unknown) --
/// cross-pack links address packs by fileId order, so item_links() needs these.
/// `progress(done,total)` is called after each pack. Returns the number of
/// (type,id) entries collected. Adds to whatever is already loaded.
size_t build(const std::string& dat_path, const std::vector<MftData>& cntc_entries,
             const std::vector<uint32_t>& file_ids,
             const std::function<void(size_t done, size_t total)>& progress);

/// One already-decompressed cntc pack, for build_from_packs().
struct PackBytes {
    uint32_t base_id = 0;
    uint32_t file_id = 0;
    std::vector<uint8_t> bytes;
};

/// build() over already-decompressed cntc packs.
size_t build_from_packs(const std::vector<PackBytes>& packs);

/// All asset fileIds a content object references, in object order (item: icon;
/// skin/outfit: model first, then icon/variants). Keyed by dataId. Empty = not found.
/// The caller classifies each fileId (texture/model/audio) via the dat/index.
const std::vector<uint32_t>& resolve_all(uint32_t content_type, uint32_t id);

/// One appearance object an item grants: a skin, mount skin or outfit (`type` is
/// its CONTENT_TYPE_*, `id` its dataId -- look its assets up with resolve_all()).
/// `via_item` is the item inside a container it came from, 0 if direct.
struct ContentLink {
    uint32_t type = 0;
    uint32_t id = 0;
    uint32_t via_item = 0;
};

/// What an item grants, in field order: its own skin (weapons/armor/backs), an
/// unlock consumable's skin / mount skin / outfit, and for a container the skins
/// of the items inside it. Empty if none/unknown. Part of the disk cache.
const std::vector<ContentLink>& item_links(uint32_t item_id);

/// A skin's composite appearance token: the u64 at +208 of its content object,
/// the key into the Composite file's per-race armor entries (composite.h).
/// 0 when the skin is unknown or carries none. Part of the disk cache.
uint64_t skin_token(uint32_t skin_id);

/// The object's first asset fileId (item: icon; skin/outfit: model).
uint32_t resolve(uint32_t content_type, uint32_t id);
inline uint32_t resolve_item(uint32_t item_id) { return resolve(CONTENT_TYPE_ITEM, item_id); }

/// The baseId of the cntc pack the (content_type,id) object was found in, so a
/// caller can re-fetch and decompress just that one entry for a deeper, typed
/// decode (see castlemist::cschema in content_schema.h) than the flat fileId
/// list above carries. 0 if not found.
///
/// Only populated by an in-session build() -- unlike resolve_all()/resolve(),
/// it is NOT part of the on-disk cache (save()/load()), so it comes back 0
/// after a load() until build() actually runs again this session.
uint32_t content_base_id(uint32_t content_type, uint32_t id);

/// The codename slug of a content object that references `file_id` as an asset
/// (its icon, model, sound, ...), e.g. "vl8Av.4gynM" -- ArenaNet's internal
/// identifier, not a localized display name (see content_store.cpp's
/// ContentObject::name for the same field on a directly-previewed cntc entry).
/// Empty when unknown, including whenever build() hasn't run this session --
/// like content_base_id(), this is session-only and not part of the disk cache.
const std::string& name_for_fileid(uint32_t file_id);

/// Simple binary cache (magic + records). Lets the map survive across sessions.
bool save(const std::wstring& path);
bool load(const std::wstring& path);

} // namespace castlemist::cmap

#endif // CONTENT_MAP_H
