/// @file
/// @brief The Map window (Tools > Map Window...): a top-level window that
///        loads one map into a castlemist::world::WorldScene on worker threads
///        and lets the user fly or walk it in a castlemist::render::WorldView.
///
/// Also the window's pure input maths -- the fly camera and the walk delta --
/// so the ui suite can test them without a window. They follow the rules of
/// the old fly view (src/render/flycam.cpp) but keep their state in a
/// FlyState instead of that view's globals.
///
/// @warning Not a public header -- internal to `src/ui/`.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "castlemist/render/world_view.h"
#include "castlemist/render/world_prep.h"

namespace castlemist::ui {

/// @brief Index of each movement key in the `keys[8]` arrays below
///        (the same order as the old fly view's FlyKey).
enum MapWinKey { MW_FWD = 0, MW_LEFT, MW_BACK, MW_RIGHT, MW_DOWN, MW_UP, MW_FAST, MW_SLOW, MW_KEY_COUNT };

/// @brief MapWinInput: the movement key a virtual-key code drives, or -1.
///        W/A/S/D move, Q down, E up, Shift fast, Ctrl slow.
int map_win_key(unsigned vk);

/// @brief A free camera in map space (up = -Z) with an explicit basis.
struct FlyState {
    float pos[3] = {0, 0, 0};
    float fwd[3] = {1, 0, 0};
    float up[3] = {0, 0, -1};
    float speed = 1000;   ///< map units per second
};

/// @brief Camera right (up x fwd, unit).
void fly_right(const FlyState& s, float out[3]);

/// @brief Moves the camera for one frame: W/S along `fwd`, A/D along right,
///        Q/E along WORLD up (-Z), never the rolled camera up. dt is clamped to
///        0.1 s; Shift scales the speed by 6, Ctrl by 0.15. True when it moved.
bool fly_step(FlyState& s, const bool keys[8], float dt);

/// @brief Mouse look: yaw about the camera's own up, pitch about its own right
///        (0.0032 rad per pixel; drag right turns right, drag down looks down).
///        No clamp: the view rotates through vertical without flipping.
void fly_mouse(FlyState& s, float dx, float dy);

/// @brief Wheel: speed *= 1.25^notches, clamped to [1, 200000].
void fly_adjust_speed(FlyState& s, float notches);

/// @brief Walk mode's horizontal step for one frame: the camera forward and
///        right projected on XY (each re-normalised), summed per the keys,
///        scaled by `walkSpeed` (Shift x6, Ctrl x0.15) and the clamped dt.
///        Q/E are ignored. Zero when nothing is held.
void walk_delta(const FlyState& s, const bool keys[8], float walkSpeed, float dt, float out[2]);

/// @brief What the headless GW2_MAPWIN hook wants from the window: once every
///        model is loaded or failed (or after 600 s), render at 1280x720, save
///        `shotPath`, write one stats line to `statsPath`, close `quitWindow`.
struct MapWinHeadless {
    std::string shotPath;
    std::string statsPath;
    HWND quitWindow = nullptr;
    bool geometry = false;        ///< GW2_MAPWIN_SHADING=geometry
    bool shader = false;          ///< GW2_MAPWIN_SHADING=shader (waits for the shader pool too)
    /// GW2_MAPWIN_SKY=day|night|off and/or approx|proven (comma separated,
    /// parse_world_sky): the sky's time of day (Day when unset) and which bake
    /// it shows (the approximate sky when unset).
    castlemist::render::WorldSkyTime sky = castlemist::render::WorldSkyTime::Day;
    castlemist::render::WorldSkyKind skyKind = castlemist::render::WorldSkyKind::Approx;
    bool fixedTime = false;       ///< GW2_MAPWIN_TIME set: the clock stays at `time`
    double time = 0;
    bool focusAnimated = false;   ///< GW2_MAPWIN_FOCUS=animated: frame the nearest playing placement
    /// GW2_MAPWIN_FOCUS=props: a low camera over the densest cluster of props,
    /// looking at the horizon (WorldView::focus_props).
    bool focusProps = false;
    /// Frames timed after the shot (WorldView::bench), for the stats line.
    int benchFrames = 30;
    /// GW2_MAPWIN_SHADER_RANGE / GW2_MAPWIN_SHADER_MAX: override
    /// WorldMaterialSettings::shaderRange / shaderMaxInstances (measuring
    /// the Shader pass at other scales); <= 0 / < 0 keeps the default.
    float shaderRange = 0;
    int shaderMax = -1;
    /// GW2_MAPWIN_FOLIAGE=off|colliders|all (mapwin_foliage_from_text):
    /// -1 unset (the window's default: on, Colliders), 0 off, 1 Colliders, 2 All.
    int foliage = -1;
    /// GW2_MAPWIN_FOCUS=zonetree: frame the placed Collider instance nearest
    /// the start camera (WorldView::focus_zone_tree), Collision on, and log
    /// its screen rect as `treeRect`.
    bool focusZoneTree = false;
    bool focusHorizon = false;    ///< GW2_MAPWIN_FOCUS=horizon: the framed camera, levelled (mapwin_level_camera)
    /// GW2_MAPWIN_FOCUS=foliage (0.9) / foliagemax (1) / forest (1, Collider
    /// instances only): a ground-level view into the foliage tile at that
    /// quantile of placed density (WorldView::focus_foliage); < 0 = not asked.
    float focusFoliage = -1;
    bool focusForest = false;
    /// GW2_MAPWIN_FOLIAGE_SCALE: WorldView::set_foliage_distance_scale (the
    /// `foliageDistanceScale` view setting); <= 0 = unset (1).
    float foliageScale = 0;
};

/// @brief GW2_MAPWIN_FOLIAGE's value (any case): "off" -> 0, "colliders" -> 1,
///        "all" -> 2; anything else -> -1 (unset).
int mapwin_foliage_from_text(const std::string& text);

/// @brief Status-line text: L"fol %d/%d" (foliageVisible / foliagePlaced).
std::wstring mapwin_foliage_status(const castlemist::render::WorldViewStats& st);

/// @brief GW2_MAPWIN_SHADING's value: "geometry" -> Geometry, "shader" ->
///        Shader (any case); anything else -> Materials.
castlemist::render::WorldShading mapwin_shading_from_text(const std::string& text);

/// @brief Status-line text for Shader shading: L"shader %d inst" and, when
///        any fell back, L" (%d to Materials: %d pending, %d no shader, %d
///        animated, %d over cap)"; empty unless @p shading is Shader.
std::wstring mapwin_shader_status(castlemist::render::WorldShading shading, const castlemist::render::WorldViewStats& st);

/// @brief Levels @p c: forward loses its vertical (z) part and up becomes
///        -Z, position kept; straight up or down looks along +X.
void mapwin_level_camera(castlemist::render::WorldCamera& c);

/// @brief The Sky button's next time of day: Day -> Night -> Off -> Day.
castlemist::render::WorldSkyTime mapwin_next_sky(castlemist::render::WorldSkyTime t);

/// @brief "geometry" (any case) -> true; anything else -> false (materials).
bool mapwin_shading_is_geometry(const std::string& text);

/// @brief Status-line text: L"tex %d (%.1f MB)  anim %d/%d" (animated / unresolvedClips).
std::wstring mapwin_material_status(const castlemist::render::WorldViewStats& st);

/// @brief One clock step: unchanged when @p motionOn is false; else adds dt clamped to [0, 0.1] s
///        (a stalled timer, e.g. a window drag, must not make the animation jump).
double mapwin_clock_step(double clock, float dt, bool motionOn);

/// @brief Opens the Map window on @p mapFileId (closing one already open).
///        The window is top-level and unowned; @p owner only parents message boxes.
///        Needs a loaded dat (g_app->data_gw2); the loader parses its own
///        copy from the same path, which the model pool then shares.
///        @p headless is copied.
void open_map_window(HWND owner, uint32_t mapFileId, const MapWinHeadless* headless = nullptr);

/// @brief Tools > Map Window...: asks for a map fileId (default: the selected
///        entry's fileId when it is a map) and opens the window on it.
void prompt_map_window(HWND owner);

/// @brief Destroys the Map window, if one is open (its WM_DESTROY bumps the
///        worker generation). The main window calls it from its own
///        WM_DESTROY: the Map window is not owned, so it is not destroyed
///        with the main window.
void close_map_window();

} // namespace castlemist::ui
