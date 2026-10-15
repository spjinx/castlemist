/// @file
/// @brief cm_world_export_fixture: writes the world export's test fixture for
///        the Blender ctests (tests/CMakeLists.txt).
///
///     cm_world_export_fixture <dir> [--probe-only]
///
/// Writes `<dir>/probe.glb` (exportgltf::write_frame_probe). `--probe-only`
/// writes nothing else; later world-export tasks add the rest of a fixture
/// export behind the default mode.

#include "castlemist/exportgltf/world_probe.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cm_world_export_fixture <dir> [--probe-only]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::string err;
    if (!castlemist::exportgltf::write_frame_probe((dir / "probe.glb").string(), &err)) {
        std::fprintf(stderr, "probe: %s\n", err.c_str());
        return 1;
    }
    std::printf("FIXTURE probe %s\n", (dir / "probe.glb").string().c_str());
    return 0;
}
