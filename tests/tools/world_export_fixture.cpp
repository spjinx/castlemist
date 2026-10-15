/// @file
/// @brief cm_world_export_fixture: writes the world export's test fixture for
///        the Blender ctests (tests/CMakeLists.txt).
///
///     cm_world_export_fixture <dir> [--probe-only | --skinned-probe]
///
/// Default: `<dir>` is cleared and gets the whole synthetic export
/// (tests/world_export_synthetic.h through exportgltf::export_world_folder):
/// manifest.json, probe.glb, cells/, models/. `--probe-only` writes
/// `<dir>/probe.glb` (exportgltf::write_frame_probe) and nothing else.
/// `--skinned-probe` clears `<dir>` and writes test_world::skinned_probe() as
/// the world export writes a model (write_vrchat_folder, map_to_gltf_root(1)):
/// `<dir>/skinned_probe.glb` and its materials.json.

#include "castlemist/exportgltf/vrchat_export.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_folder.h"
#include "castlemist/exportgltf/world_probe.h"

#include "../world_export_synthetic.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cm_world_export_fixture <dir> [--probe-only | --skinned-probe]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const bool probeOnly = argc > 2 && std::strcmp(argv[2], "--probe-only") == 0;
    if (probeOnly) {
        std::string err;
        if (!castlemist::exportgltf::write_frame_probe((dir / "probe.glb").string(), &err)) {
            std::fprintf(stderr, "probe: %s\n", err.c_str());
            return 1;
        }
        std::printf("FIXTURE probe %s\n", (dir / "probe.glb").string().c_str());
        return 0;
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (argc > 2 && std::strcmp(argv[2], "--skinned-probe") == 0) {
        castlemist::exportgltf::VrchatFolderOptions vo;
        vo.gltf.rootMatrix = castlemist::exportgltf::map_to_gltf_root(1.0);
        const auto r = castlemist::exportgltf::write_vrchat_folder(castlemist::test_world::skinned_probe(),
                                                                   dir.string(), "skinned_probe", 0, vo);
        if (!r.ok) {
            std::fprintf(stderr, "skinned probe: %s\n", r.error.c_str());
            return 1;
        }
        std::printf("FIXTURE skinned probe %s\n", r.glb.c_str());
        return 0;
    }
    const castlemist::world::WorldScene scene = castlemist::test_world::synthetic_world();
    castlemist::test_world::SyntheticModelSource src;
    castlemist::exportgltf::WorldFolderOptions opts;
    opts.texelsPerChunk = 16;
    const castlemist::exportgltf::WorldFolderReport r =
        castlemist::exportgltf::export_world_folder(scene, src, nullptr, dir.string(), opts);
    if (!r.ok) {
        std::fprintf(stderr, "export: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("FIXTURE export %s\n", r.manifest.c_str());
    return 0;
}
