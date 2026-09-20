/// @file
/// @brief Human labels for the coarse `entries.type` and packfile `entries.container`
/// values a gw2index DB carries, so the index-mode type/container filters can show a
/// real category ("Model", "Anim Blend Tree", ...) instead of a bare fourcc -- the
/// same idea as T3D's own content-browser type list (see content_store.cpp's
/// content_type_name(), which already does this for cntc content-object types).

#pragma once

#include <string>

namespace castlemist::db {

/// Label for one of index_builder's coarse `entries.type` buckets (see
/// index_builder.cpp's classify()): "packfile", "texture", "dds", "strs", "riff",
/// "png", "jpeg", "exe", "asnd", "binary", "empty". Returns nullptr for anything
/// else, so the caller falls back to showing the raw value rather than guessing.
const char* coarse_type_name(const std::string& type);

/// Label for a packfile container fourcc (the raw bytes at PF header offset 8,
/// stored as `entries.container`). Returns nullptr when this codebase has no
/// confirmed identity for the container, so the caller falls back to the raw
/// fourcc instead of guessing.
///
/// Sourced from identifications already confirmed elsewhere in this repo:
/// dumps/packfile/gw2_packfile.json (the IDA strucTabs struct registry
/// index_builder.cpp's chunk resolver consults -- e.g. container "mach" nests
/// PackAnimMachine/PackAnimMachineState/PackAnimMachineTransition/
/// PackAnimMachineAction structs, GW2's animation state-machine graph) and
/// docs/research/filetypes.md plus the container-identifying comments already
/// in entry_extractor.cpp, content_store.cpp and gw2-shaders-dxbc.md (ASND/
/// ABNK/AMSP audio, PIMG/PGTB atlases, cntc content database, AMAT/GRMT
/// materials, mapc/area maps).
const char* container_type_name(const std::string& container);

} // namespace castlemist::db
