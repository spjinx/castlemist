/// @file
/// @brief WorldView: a self-contained Direct3D 11 view that draws a
///        castlemist::world::WorldScene (the map window's surface).
///
/// It owns its own device, swap chain and targets and shares nothing with the
/// main window's renderer (`model_renderer.h`), so it can be created, resized
/// and destroyed without disturbing the preview. Map space throughout, up = -Z.
/// @ingroup render
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "castlemist/world/world_scene.h"

struct ModelPreview;   // castlemist/extract/model_types.h

namespace castlemist::render {

/// @brief What the view can draw; each can be toggled with WorldView::set_layer.
///        Collision is off after create(); the rest are on.
enum class WorldLayer { Terrain = 0, Props, Collision, Water, Count };

/// @brief A free camera in map space (up = -Z). `fwd` and `up` need not be
///        unit or exactly perpendicular; the view orthonormalizes them.
struct WorldCamera {
    float pos[3]{};
    float fwd[3]{1, 0, 0};
    float up[3]{0, 0, -1};
    float fovY = 1.15f;       ///< Vertical field of view, radians.
    float farDist = 40000;    ///< Far plane and fog end, map units.
};

/// @brief Last frame's numbers, for a status bar.
struct WorldViewStats {
    /// items drawn (terrain chunks + prop and collision instances + the water
    /// plane), draw calls, triangles (thousands)
    int visible = 0, draws = 0, ktris = 0;
    float ms = 0;                            ///< CPU time of the last render()
    /// Models uploaded / rejected by add_models since set_world; chunks = terrain chunks uploaded.
    int modelsLoaded = 0, modelsFailed = 0, chunks = 0;
    /// Collision meshes on the GPU: 0 until the Collision layer is first shown
    /// after set_world (they upload lazily, then stay).
    int collisionMeshes = 0;
};

/// @brief Draws one WorldScene into one window.
class WorldView {
public:
    WorldView();
    ~WorldView();
    WorldView(const WorldView&) = delete;
    WorldView& operator=(const WorldView&) = delete;

    /// @brief Creates the device + swap chain on @p surface (hardware, WARP
    ///        fallback) and the pipeline. False when any of it fails. Resets
    ///        everything (world, layers, stats), so call it first.
    bool create(HWND surface);
    /// @brief Resizes the swap chain and depth buffer (no-op for w or h <= 0).
    void resize(int w, int h);
    /// @brief Uploads the scene's terrain (one vertex buffer per chunk),
    ///        water plane and lighting, records every prop instance, and keeps
    ///        a CPU copy of the collision. Replaces anything set before, loaded
    ///        models included. Needs a created view (before create() nothing is
    ///        uploaded). The collision meshes upload on the first
    ///        set_layer(Collision, true) or the first render() with that layer
    ///        on (not while device_lost()); the CPU copy is then released.
    void set_world(const castlemist::world::WorldScene& s);
    /// @brief Uploads streamed prop models: each pair is (index into the
    ///        scene's `models`, its decoded preview). Instances of a model are
    ///        drawn once it has arrived. A null, out-of-range, empty or
    ///        non-finite model counts as failed; a model already added
    ///        (loaded or failed) is ignored. Does nothing before create() or
    ///        while device_lost(). The CPU half runs across cores; call it on
    ///        the thread that renders. The previews are kept (for picking).
    void add_models(const std::vector<std::pair<uint32_t, std::shared_ptr<const ModelPreview>>>& batch);
    /// @brief Culls, draws and presents one frame.
    void render(const WorldCamera& cam);
    /// @brief The prop under pixel (@p sx, @p sy) (0,0 = top left of the
    ///        surface) seen from @p cam: the index into the scene's `props`
    ///        of the nearest loaded prop whose LOD0 triangles the pixel's ray
    ///        hits, or -1 (none, or the Props layer is hidden). Terrain does
    ///        not block it. CPU only; works while device_lost().
    int pick(int sx, int sy, const WorldCamera& cam) const;
    /// @brief Shows or hides one layer (all but Collision are on after create()).
    void set_layer(WorldLayer l, bool on);
    /// @brief Whether layer @p l is shown.
    bool layer(WorldLayer l) const;
    /// @brief Numbers from the last presented frame, plus upload counts.
    WorldViewStats stats() const;
    /// @brief True after the device was removed or reset (e.g. a TDR) or the
    ///        targets could not be rebuilt on resize. render(), resize() and
    ///        save_screenshot() then do nothing; the owner should call create()
    ///        and set_world() again (create() clears the flag).
    bool device_lost() const;
    /// @brief Writes the last frame (re-drawn with the last camera) as a
    ///        24-bit BMP. False with no device, no frame yet, or on I/O failure.
    bool save_screenshot(const char* bmpPath);
    /// @brief A camera that frames the whole map (terrain, bounds and prop
    ///        positions) from above, and a fly speed
    ///        hint (map units per second) when @p speedHint is non-null.
    WorldCamera frame_all(float* speedHint) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace castlemist::render
