/// @file
/// @brief cm_world_export_fixture: writes the world export's test fixture for
///        the Blender ctests (tests/CMakeLists.txt).
///
///     cm_world_export_fixture <dir> [--probe-only]
///
/// Default: `<dir>` is cleared and gets the whole synthetic export
/// (tests/world_export_synthetic.h through exportgltf::export_world_folder):
/// manifest.json, probe.glb, cells/, models/. `--probe-only` writes
/// `<dir>/probe.glb` (exportgltf::write_frame_probe) and nothing else.

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
        std::fprintf(stderr, "usage: cm_world_export_fixture <dir> [--probe-only]\n");
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
