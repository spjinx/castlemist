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

#include <memory>

#include "castlemist/world/world_scene.h"

namespace castlemist::render {

/// @brief What the view can draw; each can be toggled with WorldView::set_layer.
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
    int visible = 0, draws = 0, ktris = 0;   ///< items drawn, draw calls, triangles (thousands)
    float ms = 0;                            ///< CPU time of the last render()
    int modelsLoaded = 0, modelsFailed = 0, chunks = 0;   ///< chunks = terrain chunks uploaded
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
    /// @brief Uploads the scene's terrain (one vertex buffer per chunk) and
    ///        lighting. Replaces anything set before. Needs a created view
    ///        (before create() no chunk is uploaded).
    void set_world(const castlemist::world::WorldScene& s);
    /// @brief Culls, draws and presents one frame.
    void render(const WorldCamera& cam);
    /// @brief Shows or hides one layer (all are on after create()).
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
    /// @brief A camera that frames the whole map from above, and a fly speed
    ///        hint (map units per second) when @p speedHint is non-null.
    WorldCamera frame_all(float* speedHint) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace castlemist::render
