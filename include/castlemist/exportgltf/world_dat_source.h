/// @file
/// @brief The dat-backed ModelSource of export_world_folder (spec
///        "Architecture", "Model loading"; sub-project 2's R13 threading rule).
/// @ingroup exportgltf
#pragma once

#include "castlemist/exportgltf/world_folder.h"
#include "castlemist/native/gw2dat.h"

#include <memory>

namespace castlemist::exportgltf {

/// @brief A ModelSource over one shared, read-only parsed dat (sub-project 2,
///        R13): every worker reads through the existing read paths, which open
///        a file handle per read.
///
/// Models: load_model_by_fileid(dat, id, ModelLoadOptions{wantGame,
/// /*decodeTextures=*/false}). Textures: decode_texture_capped, cached by
/// (fileId, maxSize) under a mutex; each is decoded once, by the first worker
/// that asks, while the others wait for it. A load that throws is a failed
/// model (null), except a world::DatIoError, which is rethrown. @p dat must
/// outlive the source.
///
/// wantGame defaults to true (measured, Task 8): a wantGame = false load has no
/// render state, and model 1766522's materials.json then differs from the
/// traced one in blend, preset, cull and emission (spec "Model loading": the
/// export uses wantGame = true when the two differ).
std::unique_ptr<ModelSource> make_dat_model_source(Gw2Dat& dat, bool wantGame = true);

} // namespace castlemist::exportgltf
