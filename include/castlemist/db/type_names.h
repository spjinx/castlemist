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
/// Every mapping is confirmed against a real gw2index build (see
/// type_names.cpp's top-of-function comment): join `entries.container` to
/// `chunks.struct_variant` and read the struct names index_builder.cpp's own
/// resolve_variant() already resolved for that exact container from
/// dumps/packfile/gw2_packfile.json, e.g. container "anic" carries chunks
/// mach/fall/seqn/cnfg resolving to PackAnimMachine/PackAnimFallback/
/// PackAnimSequence/PackAnimConfig -- GW2's animation-graph container (what
/// T3D calls an anim blend tree). This is deliberately NOT "look up a fourcc
/// in the JSON and assume its top-level key is a container" -- an earlier
/// version of this function did that and got several containers wrong (it
/// named nested chunk fourccs -- "mach", "havk", "CSCN", "GRMT", ... -- as if
/// they were containers in their own right, when the real container is
/// "anic"/"hvkC"/"CINP"/"AMAT" etc.). Two exceptions to "struct-variant
/// confirmed": txtp/Text Pack Passwords is IDA-confirmed instead
/// (docs/research/filetypes.md section 2a), since it did not happen to
/// appear in the sampled archive; "cmaC"/Collision Model Manifest and
/// "mMet"/Map Metadata are circumstantial (see type_names.cpp's comment next
/// to them) -- plausible from context, but not read out of the file the way
/// e.g. "bone" was.
const char* container_type_name(const std::string& container);

} // namespace castlemist::db
