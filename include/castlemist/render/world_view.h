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
#include <string>
#include <utility>
#include <vector>

#include "castlemist/render/world_prep.h"
#include "castlemist/world/world_scene.h"

struct ModelPreview;    // castlemist/extract/model_types.h
struct CappedTexture;   // castlemist/extract/model_types.h

namespace castlemist::render {

/// @brief What the view can draw; each can be toggled with WorldView::set_layer.
///        Collision is off after create(); the rest are on.
enum class WorldLayer { Terrain = 0, Props, Collision, Water, Count };

/// @brief How the view shades. Geometry is sub-project 2's look (flat tint,
///        hemisphere + sun, baked AO); Materials paints what has its textures
///        (terrain: note §1.2; props: their diffuse per submesh, alpha as the
///        glTF export decides it) and falls back to Geometry for the rest.
///        Shader draws the static props near the camera with the game's own
///        bgfx DXBC material shaders (the main window's Shader mode, lit by
///        the map's env rig) and everything else -- terrain, water, animated
///        props, props beyond WorldMaterialSettings::shaderRange or past
///        shaderMaxInstances, and props whose game materials are missing,
///        failed or still building -- in the Materials look (stats() counts
///        them). The sky (set_sky) shows under Materials and Shader only.
///        Materials after create().
enum class WorldShading { Geometry, Materials, Shader };

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
    /// The texture cache: textures on the GPU, fileIds whose decode or upload
    /// failed, and the GPU size of the ready ones (MB, all mips). Cleared by
    /// set_world and create().
    int textures = 0, texturesFailed = 0;
    float textureMB = 0;
    /// Terrain chunks drawn last frame in the AO look (unresolved material,
    /// no pages, a colour texture or page pending or failed, or Geometry shading).
    int chunksUntextured = 0;
    /// Prop sub-range draws last frame in the AO look under Materials shading:
    /// no diffuse, a diffuse on UV2+, or its texture pending or failed. 0
    /// under Geometry, which draws one range per (model, LOD) bucket.
    int materialsUntextured = 0;
    /// Loaded prop materials whose diffuse scrolls (texTransforms, note section 4).
    int scrollingMaterials = 0;
    /// Loaded placements that play their clip (spec SkinnedPropPass): a
    /// non-zero animSequence naming a clip of a model prepared with skin.
    int animated = 0;
    /// Loaded placements with a non-zero animSequence left in bind pose: it
    /// names no clip of their model (the fallback rule is UNPROVEN, note
    /// section 5), or the model has no skin stream (more than 256 joints, or
    /// prepared without skin).
    int unresolvedClips = 0;
    /// Of unresolvedClips: the placement's clip exists (animationTokens) but failed to decode.
    int clipsFailedDecode = 0;
    /// The water plane has an env preset (spec WaterPass):
    /// `environment.water[light.presetIndex]` exists. Materials shading then
    /// draws it from the preset; Geometry, or no preset, the flat sub-project
    /// 2 water (no preset also adds a warnings() line, note section 6).
    bool waterPreset = false;
    /// The sky mode drawn last frame (sky_mode()), -1 for none.
    int skyMode = -1;
    /// Shader shading, last frame: prop instances drawn with game shaders,
    /// their draw calls, and the instances in range (and frustum) that fell
    /// back to the Materials look: their model's game materials are not in
    /// (not traced yet, or still building on the GPU; `shaderPending`), some
    /// submesh has none or failed to build (`shaderNoShader`), or the
    /// placement plays a clip (`shaderAnimated`). `shaderCapped`: in range
    /// but past shaderMaxInstances. All 0 under Geometry and Materials.
    int shaderInstances = 0, shaderDraws = 0;
    int shaderPending = 0, shaderNoShader = 0, shaderAnimated = 0, shaderCapped = 0;
    /// Models whose game materials are built on the GPU / failed to build
    /// (cumulative since set_world); materials that sample the light buffer
    /// and get the flat stand-in (no light pre-pass in the Map window).
    int shaderModels = 0, shaderModelsFailed = 0, shaderLightStandIns = 0;
    /// Distinct game VS / PS objects created (cache by DXBC bytes).
    int shaderPrograms = 0;
    /// UI-thread ms the last frame spent building game materials
    /// (prepare_shader), and the most any frame has since set_world.
    float shaderBuildMs = 0, shaderBuildMsMax = 0;
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
    ///        uploaded). Clears the texture cache; a resolved chunk's page with
    ///        no fileId becomes a 1x1 texture of its stored solid colour (byte
    ///        order as stored, UNPROVEN, note §1.6). The collision meshes upload on the first
    ///        set_layer(Collision, true) or the first render() with that layer
    ///        on (not while device_lost()); the CPU copy is then released.
    void set_world(const castlemist::world::WorldScene& s);
    /// @brief Uploads streamed, already prepared prop models
    ///        (prepare_world_model_shared, typically on a load worker): GPU
    ///        buffers and instance placement only, no CPU prep. Instances of a
    ///        model are drawn once it has arrived. An out-of-range model, a null
    ///        preview, or a null or not-ok prep counts as failed; a model
    ///        already added (loaded or failed) is ignored. Does nothing before
    ///        create() or while device_lost(). Call it on the thread that
    ///        renders. The previews are kept (for picking).
    void add_models(const std::vector<WorldModelLoad>& batch);
    /// @brief Uploads worker-decoded textures (decode_texture_capped) into the
    ///        view's cache, keyed by fileId: once per fileId, a decode that
    ///        failed (empty mips) is counted in `texturesFailed`, null entries
    ///        are skipped. Does nothing before create() or while device_lost().
    ///        Call it on the thread that renders. create() drops the cache with
    ///        the old device and nothing restores it.
    void add_textures(const std::vector<std::shared_ptr<const CappedTexture>>& batch);
    /// @brief As above from (index into the scene's `models`, decoded
    ///        preview) pairs: prepares them across cores on the calling
    ///        thread first, then forwards. For tests and tools; the map window
    ///        prepares on its workers instead.
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

    /// @brief Replaces the baked sky modes (bake_world_sky, world_sky.h):
    ///        one texture per mode, uploaded once. Empty clears the sky.
    ///        Does nothing before create() or while device_lost().
    void set_sky(const std::vector<WorldSkyImage>& modes);
    /// @brief The time of day the sky shows (Day after create()); takes
    ///        effect next frame.
    void set_sky_time(WorldSkyTime t);
    WorldSkyTime sky_time() const;
    /// @brief The mode the next frame draws: world_sky_mode(the baked modes,
    ///        sky_time()), and -1 under Geometry or with no sky.
    int sky_mode() const;

    /// @brief Records worker-traced game materials (WorldGameLoad): CPU data
    ///        only; the GPU objects are built by prepare_shader / render() as
    ///        the models come into range. A model already given, an
    ///        out-of-range index or a null `game` (counted failed) is
    ///        ignored after the first. Does nothing before create() or while
    ///        device_lost(). Call it on the thread that renders.
    void add_game_materials(const std::vector<WorldGameLoad>& batch);
    /// @brief Builds the game-shader GPU state (shaders from a cache by DXBC
    ///        bytes, input layouts, a GVertex buffer, blend states) of models
    ///        with an instance within shaderRange of @p cam, nearest first,
    ///        until @p budgetMs is spent (<= 0: no limit). render() calls it
    ///        under Shader with a small budget, so the UI thread never
    ///        stalls on it. Returns how many such models still wait for
    ///        their preview, their game data or this build.
    int prepare_shader(const WorldCamera& cam, double budgetMs);

    /// @brief Geometry, Materials or Shader (Materials after create()); takes effect next frame.
    void set_shading(WorldShading s);
    WorldShading shading() const;
    /// @brief Replaces the material settings (defaults after create()); takes
    ///        effect next frame. `maxTextureSize` and `waterBumpTextureIndex`
    ///        only steer what the owner decodes (world_texture_ids).
    void set_material_settings(const WorldMaterialSettings& s);
    const WorldMaterialSettings& material_settings() const;
    /// @brief The Motion toggle (on after create()). Off freezes the material clock.
    void set_motion(bool on);
    bool motion() const;
    /// @brief The material clock, seconds; the window drives it.
    void set_time(double seconds);
    /// @brief The clock set by set_time; Motion off freezes it (animating() is false), it does not reset it.
    double time() const;
    /// @brief Materials shading && motion && (a scrolling prop material, a
    ///        water preset or a playing animated placement is loaded); the
    ///        owner repaints on a timer while true.
    bool animating() const;
    /// @brief Verification hook: moves @p cam to frame the playing animated
    ///        placement (stats().animated) whose world sphere centre is
    ///        nearest @p cam's position, from 4x its radius back along @p
    ///        cam's view direction (direction and up kept). False, and @p cam
    ///        untouched, when none is loaded.
    bool focus_animated(WorldCamera& cam) const;
    /// @brief The pixel rect {left, top, right, bottom} (right and bottom
    ///        exclusive, clamped to the surface) of the projected world sphere
    ///        of the placement focus_animated would pick for @p cam. False
    ///        when none is loaded, or the sphere is wholly behind the camera
    ///        or off the surface.
    bool animated_screen_rect(const WorldCamera& cam, int rect[4]) const;
    /// @brief Verification hook: chunk @p chunk's (index into the scene's
    ///        `terrain.chunks`) §1.2 albedo only (no light, fog or gamma),
    ///        linear, into a @p size x @p size RGBA8 UNORM offscreen target
    ///        drawn orthographically top down; row 0 = north, column 0 = west.
    ///        @p rgba gets size*size*4 bytes, rows tightly packed. The swap
    ///        chain and the next frame are untouched. False when the chunk
    ///        would not draw textured, @p size <= 0, or the device is lost.
    bool capture_chunk_albedo(size_t chunk, int size, std::vector<uint8_t>& rgba);
    /// @brief One line per UNPROVEN setting or castlemist choice in effect
    ///        (material_setting_warnings), for a log or the status bar.
    std::vector<std::string> warnings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace castlemist::render
