/// @file
/// @brief Tools > Map Window...: one map in its own top-level window, loaded
///        on worker threads (map_window_loader.cpp) and drawn by a
///        castlemist::render::WorldView. Fly or walk it, toggle layers, click
///        a prop to open its model in the main window.
///
/// The pure input maths (FlyState, fly_step, fly_mouse, walk_delta) is at the
/// top so the ui suite can test it; the rest is the window.

#include "detail/map_window.h"
#include "detail/app_state.h"
#include "detail/map_window_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windowsx.h>

#include "castlemist/core/env.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/render/world_view.h"
#include "castlemist/world/stream.h"
#include "castlemist/world/walk.h"

namespace castlemist::ui {

// ---------------------------------------------------------------------------
// Pure input maths (same rules as src/render/flycam.cpp, without its globals)
// ---------------------------------------------------------------------------

namespace {

constexpr float kWorldUp[3] = {0, 0, -1};
constexpr float kLookSens = 0.0032f;   // radians per pixel
constexpr float kMaxDt = 0.1f;         // a stall must not teleport the camera
constexpr float kFastMul = 6.0f, kSlowMul = 0.15f;

void cross3(const float a[3], const float b[3], float o[3]) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool normalize3(float v[3]) {
    const float l = std::sqrt(dot3(v, v));
    if (l < 1e-8f) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

/// Rodrigues: rotate @p v about the unit @p axis by @p ang.
void rotate_about(float v[3], const float axis[3], float ang) {
    const float c = std::cos(ang), s = std::sin(ang), d = dot3(axis, v);
    float cr[3];
    cross3(axis, v, cr);
    for (int k = 0; k < 3; ++k) v[k] = v[k] * c + cr[k] * s + axis[k] * d * (1.0f - c);
}

/// Re-orthonormalises the basis so float drift cannot skew it.
void renormalize(FlyState& s) {
    if (!normalize3(s.fwd)) { s.fwd[0] = 1; s.fwd[1] = 0; s.fwd[2] = 0; }
    float right[3];
    cross3(s.up, s.fwd, right);
    if (!normalize3(right)) {
        // up and forward went parallel: rebuild from the world axis forward is least aligned with.
        const float x[3] = {1, 0, 0};
        const float* a = std::fabs(s.fwd[2]) < 0.9f ? kWorldUp : x;
        cross3(a, s.fwd, right);
        normalize3(right);
    }
    cross3(s.fwd, right, s.up);
    normalize3(s.up);
}

float speed_mul(const bool keys[8]) {
    float m = 1.0f;
    if (keys[MW_FAST]) m *= kFastMul;
    if (keys[MW_SLOW]) m *= kSlowMul;
    return m;
}

} // namespace

int map_win_key(unsigned vk) {
    switch (vk) {
    case 'W': return MW_FWD;
    case 'A': return MW_LEFT;
    case 'S': return MW_BACK;
    case 'D': return MW_RIGHT;
    case 'Q': return MW_DOWN;
    case 'E': return MW_UP;
    case VK_SHIFT: return MW_FAST;
    case VK_CONTROL: return MW_SLOW;
    default: return -1;
    }
}

void fly_right(const FlyState& s, float out[3]) {
    cross3(s.up, s.fwd, out);
    normalize3(out);
}

bool fly_step(FlyState& s, const bool keys[8], float dt) {
    if (dt <= 0) return false;
    dt = std::min(dt, kMaxDt);
    float ax = 0, ay = 0, az = 0;
    if (keys[MW_FWD]) ax += 1;
    if (keys[MW_BACK]) ax -= 1;
    if (keys[MW_RIGHT]) ay += 1;
    if (keys[MW_LEFT]) ay -= 1;
    if (keys[MW_UP]) az += 1;
    if (keys[MW_DOWN]) az -= 1;
    if (ax == 0 && ay == 0 && az == 0) return false;
    float right[3];
    fly_right(s, right);
    const float d = s.speed * speed_mul(keys) * dt;
    // Forward/strafe follow the camera; Q/E stay on WORLD up so "gain
    // altitude" means altitude even when the camera is rolled.
    for (int k = 0; k < 3; ++k) s.pos[k] += (s.fwd[k] * ax + right[k] * ay + kWorldUp[k] * az) * d;
    return true;
}

void fly_mouse(FlyState& s, float dx, float dy) {
    // Yaw about the camera's OWN up and pitch about its OWN right: neither
    // axis is ever parallel to what it rotates, so there is no pole to clamp.
    float right[3];
    fly_right(s, right);
    if (dx != 0) rotate_about(s.fwd, s.up, dx * kLookSens);   // drag right -> turn right
    if (dy != 0) {
        const float a = dy * kLookSens;                       // drag down -> look down
        rotate_about(s.fwd, right, a);
        rotate_about(s.up, right, a);
    }
    renormalize(s);
}

void fly_adjust_speed(FlyState& s, float notches) {
    s.speed = std::clamp(s.speed * std::pow(1.25f, notches), 1.0f, 200000.0f);
}

void walk_delta(const FlyState& s, const bool keys[8], float walkSpeed, float dt, float out[2]) {
    out[0] = out[1] = 0;
    if (dt <= 0) return;
    dt = std::min(dt, kMaxDt);
    float ax = 0, ay = 0;
    if (keys[MW_FWD]) ax += 1;
    if (keys[MW_BACK]) ax -= 1;
    if (keys[MW_RIGHT]) ay += 1;
    if (keys[MW_LEFT]) ay -= 1;
    if (ax == 0 && ay == 0) return;
    float right[3];
    fly_right(s, right);
    float f[2] = {s.fwd[0], s.fwd[1]}, r[2] = {right[0], right[1]};
    auto norm2 = [](float v[2]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1]);
        if (l < 1e-6f) { v[0] = v[1] = 0; return; }
        v[0] /= l; v[1] /= l;
    };
    norm2(f);
    norm2(r);
    float m[2] = {f[0] * ax + r[0] * ay, f[1] * ax + r[1] * ay};
    norm2(m);   // diagonal is not faster
    const float d = walkSpeed * speed_mul(keys) * dt;
    out[0] = m[0] * d;
    out[1] = m[1] * d;
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;
using castlemist::render::WorldLayer;

constexpr int kToolbarH = 34;
constexpr int kStatusH = 22;
constexpr int kWarnW = 460;
constexpr int kSurfaceW = 1280, kSurfaceH = 720;   // the default (and headless) surface size
constexpr float kWalkSpeed = 300;                  // map units per second, before Shift/Ctrl
constexpr double kHeadlessTimeout = 600;           // seconds
/// The model queue re-sorts around the camera at most this often, and only
/// after the camera moved this far (map units) since the last sort.
constexpr double kReorderSeconds = 1.0;
constexpr float kReorderDistance = 500;

const wchar_t* kWndClass = L"Gw2MapWindow";
const wchar_t* kSurfaceClass = L"Gw2MapWindowSurface";

struct MapWin {
    HWND wnd = nullptr, surface = nullptr, status = nullptr, warnEdit = nullptr;
    HWND bWalk = nullptr, bFly = nullptr, bTerrain = nullptr, bProps = nullptr, bColl = nullptr, bWater = nullptr, bMaterials = nullptr, bMotion = nullptr,
         bReset = nullptr, bWarn = nullptr;
    uint64_t gen = 0;
    uint32_t mapFileId = 0;
    std::string datPath;

    castlemist::render::WorldView view;
    bool viewOk = false;
    std::shared_ptr<const castlemist::world::WorldScene> scene;
    std::shared_ptr<const castlemist::world::CollisionIndex> coll;
    std::unique_ptr<castlemist::world::Walker> walker;
    /// Every model the pool delivered (kept to re-add after a lost device).
    std::vector<castlemist::render::WorldModelLoad> models;
    size_t modelsArrived = 0;
    int workers = 0, workersDone = 0;
    /// The models not yet taken by the pool; re-sorted around the camera in tick().
    std::shared_ptr<MapWinModelQueue> queue;
    float reorderPos[3] = {0, 0, 0};
    Clock::time_point lastReorder{};
    /// UI-thread time of each add_models batch (ms), for the headless stats.
    std::vector<float> addMs;
    /// Texture fileIds already being decoded (model pool + terrain thread).
    std::shared_ptr<MapWinTextureClaims> claims;
    bool terrainTexStarted = false, terrainTexDone = false;
    bool collDone = false;   ///< the collision build came back (index or error)

    FlyState fly;
    float fovY = 1.15f, farDist = 40000;
    bool walking = false;
    float walkSpeed = kWalkSpeed;
    bool keys[8] = {};
    bool dirty = true;
    double clock = 0;           /// the material clock (seconds); accumulates only while Motion is on
    bool warningsFinal = false; /// the warnings panel was refreshed after the last model

    bool rdrag = false, lpress = false;
    POINT last{}, ldown{};

    Clock::time_point opened = Clock::now(), lastTick = Clock::now();
    double worldSeconds = 0, collSeconds = 0, modelsSeconds = 0;
    std::wstring stage = L"Loading map...";
    std::wstring message;
    Clock::time_point messageUntil{};
    std::string error;

    std::optional<MapWinHeadless> headless;
    bool captured = false;
};

MapWin* g_mw = nullptr;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

castlemist::render::WorldCamera camera(const MapWin& m) {
    castlemist::render::WorldCamera c;
    for (int k = 0; k < 3; ++k) {
        c.pos[k] = m.fly.pos[k];
        c.fwd[k] = m.fly.fwd[k];
        c.up[k] = m.fly.up[k];
    }
    c.fovY = m.fovY;
    c.farDist = m.farDist;
    return c;
}

void set_camera(MapWin& m, const castlemist::render::WorldCamera& c) {
    for (int k = 0; k < 3; ++k) {
        m.fly.pos[k] = c.pos[k];
        m.fly.fwd[k] = c.fwd[k];
        m.fly.up[k] = c.up[k];
    }
    m.fovY = c.fovY;
    m.farDist = c.farDist;
    renormalize(m.fly);
    m.dirty = true;
}

void flash(MapWin& m, const std::wstring& msg, int seconds = 6) {
    m.message = msg;
    m.messageUntil = Clock::now() + std::chrono::seconds(seconds);
}

size_t models_total(const MapWin& m) { return m.scene ? m.scene->models.size() : 0; }

/// Places the toolbar, surface, warnings panel and status line.
void layout(MapWin& m) {
    RECT rc;
    GetClientRect(m.wnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const bool warn = SendMessageW(m.bWarn, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int surfH = std::max(1, H - kToolbarH - kStatusH);
    const int warnW = warn ? std::min(kWarnW, W / 2) : 0;
    MoveWindow(m.surface, 0, kToolbarH, std::max(1, W - warnW), surfH, TRUE);
    MoveWindow(m.warnEdit, W - warnW, kToolbarH, warnW, surfH, TRUE);
    ShowWindow(m.warnEdit, warn ? SW_SHOW : SW_HIDE);
    MoveWindow(m.status, 6, H - kStatusH + 3, std::max(1, W - 12), kStatusH - 4, TRUE);
}

void sync_mode_buttons(MapWin& m) {
    SendMessageW(m.bWalk, BM_SETCHECK, m.walking ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(m.bFly, BM_SETCHECK, m.walking ? BST_UNCHECKED : BST_CHECKED, 0);
}

void enter_fly(MapWin& m) {
    m.walking = false;
    sync_mode_buttons(m);
    m.dirty = true;
}

void enter_walk(MapWin& m) {
    if (!m.coll || m.coll->empty()) {
        flash(m, m.coll ? L"Walk: this map has no collision." : L"Walk: collision is still building.");
        enter_fly(m);
        return;
    }
    if (!m.walker) m.walker = std::make_unique<castlemist::world::Walker>(m.coll.get(), castlemist::world::WalkParams{});
    float p[3] = {m.fly.pos[0], m.fly.pos[1], m.fly.pos[2]};
    m.walker->reset(p);   // clear motion before the snap, so the snap's ground state stands
    if (!m.walker->snap_to_ground(p)) {
        flash(m, L"Walk: no ground under the camera; fly over the map first.");
        enter_fly(m);
        return;
    }
    for (int k = 0; k < 3; ++k) m.fly.pos[k] = p[k];
    m.walking = true;
    sync_mode_buttons(m);
    m.dirty = true;
}

void frame_map(MapWin& m) {
    if (!m.viewOk || !m.scene) return;
    float hint = 0;
    set_camera(m, m.view.frame_all(&hint));
    if (hint > 0) m.fly.speed = std::clamp(hint, 1.0f, 200000.0f);
    if (m.walking) enter_fly(m);
}

void apply_layer_buttons(MapWin& m) {
    auto on = [](HWND b) { return SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    m.view.set_layer(WorldLayer::Terrain, on(m.bTerrain));
    m.view.set_layer(WorldLayer::Props, on(m.bProps));
    m.view.set_layer(WorldLayer::Collision, on(m.bColl));
    m.view.set_layer(WorldLayer::Water, on(m.bWater));
    m.dirty = true;
}

/// The scene warnings, then the view's own (UNPROVEN settings, missing water preset).
void refresh_warnings(MapWin& m) {
    if (!m.scene) return;
    std::wstring warn;
    for (const std::string& w : m.scene->warnings) warn += widen(w) + L"\r\n";
    if (m.viewOk)
        for (const std::string& w : m.view.warnings()) warn += widen(w) + L"\r\n";
    if (warn.empty()) warn = L"(no warnings)";
    SetWindowTextW(m.warnEdit, warn.c_str());
}

void apply_shading_buttons(MapWin& m) {
    auto on = [](HWND b) { return SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    m.view.set_shading(on(m.bMaterials) ? castlemist::render::WorldShading::Materials
                                        : castlemist::render::WorldShading::Geometry);
    m.view.set_motion(on(m.bMotion));
    refresh_warnings(m);
    m.dirty = true;
}

void headless_capture(MapWin& m);

/// No device: as a first-time creation failure, a message (none when
/// headless, which writes its ok=0 stats line instead), then the window
/// closes. The close is posted, not done here, because callers of
/// render_now keep using @p m until they return.
void device_failed(MapWin& m, const char* why) {
    m.viewOk = false;
    m.error = why;
    if (m.headless) {
        if (!m.captured) headless_capture(m);
    } else {
        MessageBoxW(m.wnd, L"The Direct3D 11 device for the Map window was lost and could not be recreated.",
                    L"Map Window", MB_ICONERROR);
    }
    PostMessageW(m.wnd, WM_CLOSE, 0, 0);
}

/// After a removed device: a new device, the world again, every model kept.
void recover_device(MapWin& m) {
    if (!m.view.create(m.surface)) {
        device_failed(m, "Direct3D device lost and could not be recreated");
        return;
    }
    RECT rc;
    GetClientRect(m.surface, &rc);
    m.view.resize(rc.right, rc.bottom);
    if (m.scene) {
        m.view.set_world(*m.scene);
        if (!m.models.empty()) m.view.add_models(m.models);
    }
    apply_layer_buttons(m);
    apply_shading_buttons(m);
    if (m.headless && m.headless->fixedTime) m.view.set_time(m.headless->time);
    // create() dropped the texture cache with the old device; the decoded
    // textures were not kept, so they stay gone (terrain, props and water fall back).
    flash(m, L"The graphics device was reset; the map was reloaded onto a new one. "
             L"Textures are not restored after a device reset; reopen the window to see them.",
          10);
}

void render_now(MapWin& m) {
    if (!m.viewOk) return;
    if (m.view.device_lost()) recover_device(m);
    if (!m.viewOk || m.view.device_lost()) return;
    m.view.render(camera(m));
    m.dirty = false;
}

void update_status(MapWin& m) {
    const auto st = m.view.stats();
    std::wstring s = m.stage;
    wchar_t buf[512];
    if (m.scene) {
        swprintf(buf, 512, L"  |  models %d/%zu", st.modelsLoaded + st.modelsFailed, models_total(m));
        s += buf;
        if (st.modelsFailed) {
            swprintf(buf, 512, L" (%d failed)", st.modelsFailed);
            s += buf;
        }
        s += L"  |  " + mapwin_material_status(st);
        swprintf(buf, 512, L"  |  vis %d  draws %d  %d ktri  %.1f ms", st.visible, st.draws, st.ktris, st.ms);
        s += buf;
    }
    swprintf(buf, 512, L"  |  pos %.0f, %.0f, %.0f  |  %ls  speed %.0f", m.fly.pos[0], m.fly.pos[1], m.fly.pos[2],
             m.walking ? L"Walk" : L"Fly", m.walking ? m.walkSpeed : m.fly.speed);
    s += buf;
    if (!m.message.empty() && Clock::now() < m.messageUntil) s += L"  |  " + m.message;
    wchar_t cur[1024];
    GetWindowTextW(m.status, cur, 1024);
    if (s != cur) SetWindowTextW(m.status, s.c_str());
}

/// GW2_MAPWIN: one frame at 1280x720, the BMP, one stats line, then quit.
void headless_capture(MapWin& m) {
    m.captured = true;
    const double elapsed = std::chrono::duration<double>(Clock::now() - m.opened).count();
    bool shot = false;
    int animRect[4] = {-1, -1, -1, -1};
    if (m.viewOk && m.scene) {
        MoveWindow(m.surface, 0, kToolbarH, kSurfaceW, kSurfaceH, FALSE);
        m.view.resize(kSurfaceW, kSurfaceH);
        // The capture camera: the whole map, or (GW2_MAPWIN_FOCUS=animated)
        // framed on the nearest playing animated placement.
        float hint = 0;
        castlemist::render::WorldCamera cam = m.view.frame_all(&hint);
        if (m.headless->focusAnimated) m.view.focus_animated(cam);
        set_camera(m, cam);
        if (m.headless->fixedTime) m.view.set_time(m.headless->time);
        render_now(m);
        shot = m.view.save_screenshot(m.headless->shotPath.c_str());
        int r[4];
        if (m.view.animated_screen_rect(camera(m), r))
            for (int k = 0; k < 4; ++k) animRect[k] = r[k];
    }
    const auto st = m.view.stats();
    std::vector<float> add = m.addMs;
    std::sort(add.begin(), add.end());
    const float addMax = add.empty() ? 0.0f : add.back();
    const float addMedian = add.empty() ? 0.0f : add[add.size() / 2];
    char line[2048];
    std::snprintf(line, sizeof line,
                  "map=%u ok=%d models=%zu loaded=%d failed=%d props=%zu visible=%d draws=%d ktris=%d ms=%.2f "
                  "chunks=%d collisionTris=%zu warnings=%zu world_s=%.2f collision_s=%.2f models_s=%.2f total_s=%.2f "
                  "add_batches=%zu add_ms_median=%.2f add_ms_max=%.2f "
                  "shading=%s time=%.2f textures=%d textureMB=%.1f texturesFailed=%d animated=%d unresolvedClips=%d clipsFailedDecode=%d "
                  "chunksUntextured=%d materialsUntextured=%d waterPreset=%d animRect=%d,%d,%d,%d "
                  "shot=%d timeout=%d error=\"%s\"\n",
                  m.mapFileId, (m.scene && m.viewOk) ? 1 : 0, models_total(m), st.modelsLoaded, st.modelsFailed,
                  m.scene ? m.scene->props.size() : size_t(0), st.visible, st.draws, st.ktris, st.ms, st.chunks,
                  m.coll ? m.coll->triangle_count() : size_t(0), m.scene ? m.scene->warnings.size() : size_t(0),
                  m.worldSeconds, m.collSeconds, m.modelsSeconds, elapsed, add.size(), addMedian, addMax,
                  m.view.shading() == castlemist::render::WorldShading::Geometry ? "geometry" : "materials",
                  m.view.time(), st.textures, st.textureMB, st.texturesFailed, st.animated, st.unresolvedClips, st.clipsFailedDecode,
                  st.chunksUntextured, st.materialsUntextured, st.waterPreset ? 1 : 0, animRect[0], animRect[1],
                  animRect[2], animRect[3], shot ? 1 : 0,
                  elapsed >= kHeadlessTimeout ? 1 : 0, m.error.c_str());
    if (std::ofstream f(m.headless->statsPath, std::ios::binary); f) f << line;
    if (m.headless->quitWindow) PostMessageW(m.headless->quitWindow, WM_CLOSE, 0, 0);
}

void maybe_capture(MapWin& m) {
    if (!m.headless || m.captured) return;
    const double elapsed = std::chrono::duration<double>(Clock::now() - m.opened).count();
    const bool failed = !m.error.empty() && !m.scene;
    const bool modelsDone = m.scene && (m.modelsArrived >= models_total(m) ||
                                        (m.workers > 0 && m.workersDone >= m.workers));
    const bool texturesDone = !m.terrainTexStarted || m.terrainTexDone;
    if (failed || (modelsDone && m.collDone && texturesDone) || elapsed >= kHeadlessTimeout) headless_capture(m);
}

void on_world(MapWin& m, std::unique_ptr<MapWinWorldMsg> r) {
    m.worldSeconds = r->seconds;
    if (!r->scene) {
        m.error = r->error;
        m.stage = L"Load failed: " + widen(r->error);
        SetWindowTextW(m.warnEdit, m.stage.c_str());
        update_status(m);
        maybe_capture(m);
        return;
    }
    m.scene = std::move(r->scene);
    if (m.viewOk) {
        m.view.set_world(*m.scene);
        apply_layer_buttons(m);
        frame_map(m);
    }
    refresh_warnings(m);
    wchar_t buf[256];
    swprintf(buf, 256, L"Map %u: world in %.1f s, %zu models, %zu props, %zu warnings; loading models...", m.mapFileId,
             r->seconds, m.scene->models.size(), m.scene->props.size(), m.scene->warnings.size());
    m.stage = buf;
    mapwin_start_collision(m.wnd, m.gen, m.scene);
    // Textures: the terrain thread starts first so it claims the terrain's;
    // each model worker then decodes the diffuse textures nobody claimed yet.
    m.claims = std::make_shared<MapWinTextureClaims>();
    if (r->dat) {
        std::shared_ptr<Gw2Dat> dat = r->dat;   // copied: the model pool takes r->dat below
        mapwin_start_terrain_textures(m.wnd, m.gen, std::move(dat), m.scene, m.claims);
        m.terrainTexStarted = true;
    }
    if (!m.scene->models.empty()) {
        m.queue = std::make_shared<MapWinModelQueue>(m.scene, m.fly.pos);
        for (int k = 0; k < 3; ++k) m.reorderPos[k] = m.fly.pos[k];
        m.lastReorder = Clock::now();
        m.workers = mapwin_start_models(m.wnd, m.gen, std::move(r->dat), m.queue, m.claims);
    }
    m.dirty = true;
    update_status(m);
    maybe_capture(m);
}

void on_collision(MapWin& m, std::unique_ptr<MapWinCollisionMsg> r) {
    m.collDone = true;
    m.coll = std::move(r->index);
    m.collSeconds = r->seconds;
    m.walker.reset();
    const bool any = m.coll && !m.coll->empty();
    EnableWindow(m.bWalk, any ? TRUE : FALSE);
    if (!r->error.empty()) {
        flash(m, L"Collision build failed: " + widen(r->error) + L"; Walk is off.", 30);
        return;
    }
    wchar_t buf[160];
    if (any)
        swprintf(buf, 160, L"Collision ready (%zu triangles, %.1f s): Walk is available (Tab).", m.coll->triangle_count(),
                 r->seconds);
    else
        swprintf(buf, 160, L"This map has no collision: Walk is off.");
    flash(m, buf, 8);
}

/// The terrain-texture thread's batches: uploaded (and timed) like a model batch.
void on_textures(MapWin& m, std::unique_ptr<MapWinTexturesMsg> r) {
    if (!r->textures.empty() && m.viewOk) {
        const auto t0 = Clock::now();
        m.view.add_textures(r->textures);
        m.addMs.push_back(std::chrono::duration<float, std::milli>(Clock::now() - t0).count());
        m.dirty = true;
    }
    if (r->done) m.terrainTexDone = true;
    maybe_capture(m);
}

void on_models(MapWin& m, std::unique_ptr<MapWinModelsMsg> r) {
    if (!r->batch.empty() || !r->textures.empty()) {
        if (m.viewOk) {
            // Textures first, so the batch's models find theirs; both count
            // in the batch's UI-thread time. The textures are not kept (a
            // device reset does not restore them).
            const auto t0 = Clock::now();
            m.view.add_textures(r->textures);
            m.view.add_models(r->batch);
            m.addMs.push_back(std::chrono::duration<float, std::milli>(Clock::now() - t0).count());
        }
        m.modelsArrived += r->batch.size();
        for (auto& p : r->batch) m.models.push_back(std::move(p));
        m.dirty = true;
    }
    if (r->workerDone) ++m.workersDone;
    if (m.scene && (m.modelsArrived >= models_total(m) || (m.workers && m.workersDone >= m.workers))) {
        if (m.modelsSeconds == 0)
            m.modelsSeconds = std::chrono::duration<double>(Clock::now() - m.opened).count() - m.worldSeconds;
        const auto st = m.view.stats();
        wchar_t buf[256];
        swprintf(buf, 256, L"Map %u: %d models loaded, %d failed (models %.1f s after the world)", m.mapFileId,
                 st.modelsLoaded, st.modelsFailed, m.modelsSeconds);
        m.stage = buf;
        if (!m.warningsFinal) {   // once the last model is in, the view's warnings are settled
            m.warningsFinal = true;
            refresh_warnings(m);
        }
    }
    maybe_capture(m);
}

void tick(MapWin& m) {
    const auto now = Clock::now();
    const float dt = std::chrono::duration<float>(now - m.lastTick).count();
    m.lastTick = now;
    if (m.scene) {
        if (m.walking && m.walker) {
            float hd[2];
            walk_delta(m.fly, m.keys, m.walkSpeed, dt, hd);
            const float before[3] = {m.fly.pos[0], m.fly.pos[1], m.fly.pos[2]};
            const castlemist::world::StepResult r = m.walker->step(m.fly.pos, hd, dt);
            if (r.gaveUp) {
                flash(m, L"Walk: fell through the map and found no ground to recover to; back to Fly.", 10);
                enter_fly(m);
            } else if (r.recovered) {
                flash(m, L"Walk: fell through; returned to the last safe spot.");
            }
            if (before[0] != m.fly.pos[0] || before[1] != m.fly.pos[1] || before[2] != m.fly.pos[2]) m.dirty = true;
        } else if (fly_step(m.fly, m.keys, dt)) {
            m.dirty = true;
        }
    }
    // Stream around the live camera: re-sort what the pool has not taken yet.
    if (m.queue && now - m.lastReorder >= std::chrono::duration<double>(kReorderSeconds)) {
        const float dx = m.fly.pos[0] - m.reorderPos[0], dy = m.fly.pos[1] - m.reorderPos[1],
                    dz = m.fly.pos[2] - m.reorderPos[2];
        if (dx * dx + dy * dy + dz * dz > kReorderDistance * kReorderDistance) {
            if (m.queue->queue.remaining() == 0) {
                m.queue.reset();   // all taken; the workers keep their own reference
            } else {
                m.queue->queue.reorder(m.fly.pos);
                for (int k = 0; k < 3; ++k) m.reorderPos[k] = m.fly.pos[k];
            }
        }
        m.lastReorder = now;
    }
    // The material clock. A fixed headless time was set once at open and never
    // changes, so nothing to do; otherwise it runs while Motion is on.
    if (m.viewOk && !(m.headless && m.headless->fixedTime)) {
        m.clock = mapwin_clock_step(m.clock, dt, SendMessageW(m.bMotion, BM_GETCHECK, 0, 0) == BST_CHECKED);
        m.view.set_time(m.clock);
        if (m.view.animating()) m.dirty = true;
    }
    if (m.dirty && IsWindowVisible(m.wnd) && !IsIconic(m.wnd)) render_now(m);
}

void clear_keys(MapWin& m) {
    for (bool& k : m.keys) k = false;
}

void pick_at(MapWin& m, int x, int y) {
    if (!m.scene || !m.viewOk) return;
    const int i = m.view.pick(x, y, camera(m));
    if (i < 0 || static_cast<size_t>(i) >= m.scene->props.size()) {
        flash(m, L"Nothing picked (props must be loaded and the Props layer on).");
        return;
    }
    const uint32_t mi = m.scene->props[static_cast<size_t>(i)].model;
    const uint32_t fid = mi < m.scene->models.size() ? m.scene->models[mi].fileId : 0;
    wchar_t buf[160];
    swprintf(buf, 160, L"Picked prop %d: model fileId %u (opened in the main window).", i, fid);
    flash(m, buf, 10);
    navigate_to_file_id(fid);
}

LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    MapWin* m = g_mw;
    if (!m || m->surface != hwnd) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        m->dirty = true;
        return 0;
    }
    case WM_SIZE:
        if (m->viewOk) m->view.resize(LOWORD(lparam), HIWORD(lparam));
        m->dirty = true;
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        const bool down = msg == WM_KEYDOWN;
        const int k = map_win_key(static_cast<unsigned>(wparam));
        if (k >= 0) m->keys[k] = down;
        if (down && !(lparam & (1 << 30))) {   // first press, not auto-repeat
            switch (wparam) {
            case VK_TAB: if (m->walking) enter_fly(*m); else enter_walk(*m); break;
            case VK_SPACE: if (m->walking && m->walker) m->walker->jump(); break;
            case 'F': frame_map(*m); break;
            }
        }
        return 0;
    }
    case WM_KILLFOCUS: clear_keys(*m); return 0;
    case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        m->rdrag = true;
        m->last = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        return 0;
    case WM_RBUTTONUP:
        m->rdrag = false;
        if (!m->lpress) ReleaseCapture();
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        m->lpress = true;
        m->ldown = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        const bool was = m->lpress;
        m->lpress = false;
        if (!m->rdrag) ReleaseCapture();
        const int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
        if (was && std::abs(x - m->ldown.x) <= 4 && std::abs(y - m->ldown.y) <= 4) pick_at(*m, x, y);
        return 0;
    }
    case WM_CAPTURECHANGED:
        m->rdrag = m->lpress = false;
        return 0;
    case WM_MOUSEMOVE:
        if (m->rdrag) {
            const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const float dx = static_cast<float>(p.x - m->last.x), dy = static_cast<float>(p.y - m->last.y);
            m->last = p;
            if (dx != 0 || dy != 0) {
                fly_mouse(m->fly, dx, dy);
                m->dirty = true;
            }
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        if (m->walking) m->walkSpeed = std::clamp(m->walkSpeed * std::pow(1.25f, notches), 20.0f, 20000.0f);
        else fly_adjust_speed(m->fly, notches);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void destroy_state(HWND hwnd) {
    if (!g_mw || g_mw->wnd != hwnd) return;
    if (g_mw->headless && !g_mw->captured) {
        // Closed (by hand, or with the main window) before the capture: say
        // so in the stats line rather than leaving no trace of the run.
        g_mw->error = "window closed before the capture";
        g_mw->viewOk = false;   // no render while being destroyed; ok=0
        headless_capture(*g_mw);
    }
    mapwin_next_generation();   // stop every worker of this window; late posts are dropped
    // Payloads already queued for this window would be discarded with it,
    // unfreed: take them out now. (Range: WORLD..TEXTURES, all four.)
    static_assert(WM_APP_MAPWIN_MODELS > WM_APP_MAPWIN_WORLD && WM_APP_MAPWIN_COLLISION > WM_APP_MAPWIN_MODELS &&
                  WM_APP_MAPWIN_TEXTURES == WM_APP_MAPWIN_COLLISION + 1);
    MSG q;
    while (PeekMessageW(&q, hwnd, WM_APP_MAPWIN_WORLD, WM_APP_MAPWIN_TEXTURES, PM_REMOVE))
        mapwin_free_payload(q.message, q.lParam);
    KillTimer(hwnd, TIMER_MW_FRAME);
    KillTimer(hwnd, TIMER_MW_STATUS);
    delete g_mw;
    g_mw = nullptr;
}

/// Takes ownership of a worker payload; null unless it belongs to this window.
template <class T>
std::unique_ptr<T> take(LPARAM lparam, const MapWin* m) {
    std::unique_ptr<T> p(reinterpret_cast<T*>(lparam));
    if (!p || !m || p->gen != m->gen || mapwin_generation() != m->gen) return nullptr;
    return p;
}

LRESULT CALLBACK MapWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    MapWin* m = (g_mw && g_mw->wnd == hwnd) ? g_mw : nullptr;
    switch (msg) {
    case WM_APP_MAPWIN_WORLD:
        if (auto r = take<MapWinWorldMsg>(lparam, m)) on_world(*m, std::move(r));
        return 0;
    case WM_APP_MAPWIN_COLLISION:
        if (auto r = take<MapWinCollisionMsg>(lparam, m)) on_collision(*m, std::move(r));
        return 0;
    case WM_APP_MAPWIN_MODELS:
        if (auto r = take<MapWinModelsMsg>(lparam, m)) on_models(*m, std::move(r));
        return 0;
    case WM_APP_MAPWIN_TEXTURES:
        if (auto r = take<MapWinTexturesMsg>(lparam, m)) on_textures(*m, std::move(r));
        return 0;
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) return 0;   // keep the surface (and swap chain) at its size
        if (m) layout(*m);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lparam);
        mm->ptMinTrackSize = {640, 360};
        return 0;
    }
    case WM_TIMER:
        if (!m) return 0;
        if (wparam == TIMER_MW_FRAME) tick(*m);
        else if (wparam == TIMER_MW_STATUS) {
            update_status(*m);
            maybe_capture(*m);
        }
        return 0;
    case WM_COMMAND:
        if (!m) break;
        switch (LOWORD(wparam)) {
        case ID_MW_WALK: enter_walk(*m); break;
        case ID_MW_FLY: enter_fly(*m); break;
        case ID_MW_TERRAIN:
        case ID_MW_PROPS:
        case ID_MW_COLLISION:
        case ID_MW_WATER: apply_layer_buttons(*m); break;
        case ID_MW_MATERIALS:
        case ID_MW_MOTION: apply_shading_buttons(*m); break;
        case ID_MW_RESET: frame_map(*m); break;
        case ID_MW_WARNINGS: layout(*m); break;   // the panel never keeps focus
        default: return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        SetFocus(m->surface);   // keys (WASD, Space) go back to the view after any toolbar command
        return 0;
    case WM_ACTIVATE: {
        const LRESULT r = DefWindowProcW(hwnd, msg, wparam, lparam);
        if (m && LOWORD(wparam) != WA_INACTIVE) SetFocus(m->surface);
        return r;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: destroy_state(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void register_classes() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = MapWndProc;
    wc.hInstance = g_hinstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(g_hinstance, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    WNDCLASSW sc{};
    sc.lpfnWndProc = SurfaceProc;
    sc.hInstance = g_hinstance;
    sc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    sc.hbrBackground = nullptr;   // Direct3D paints it
    sc.lpszClassName = kSurfaceClass;
    RegisterClassW(&sc);
    registered = true;
}

} // namespace

bool mapwin_shading_is_geometry(const std::string& text) {
    if (text.size() != 8) return false;
    static const char kWord[] = "geometry";
    for (size_t i = 0; i < 8; ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) != kWord[i]) return false;
    return true;
}

double mapwin_clock_step(double clock, float dt, bool motionOn) {
    if (!motionOn) return clock;
    return clock + std::clamp(dt, 0.0f, 0.1f);
}

std::wstring mapwin_material_status(const castlemist::render::WorldViewStats& st) {
    wchar_t buf[96];
    swprintf(buf, 96, L"tex %d (%.1f MB)  anim %d/%d", st.textures, st.textureMB, st.animated, st.unresolvedClips);
    std::wstring s = buf;
    if (st.texturesFailed > 0) s += L" (" + std::to_wstring(st.texturesFailed) + L" failed)";
    return s;
}

void close_map_window() {
    if (g_mw) DestroyWindow(g_mw->wnd);
}

void open_map_window(HWND owner, uint32_t mapFileId, const MapWinHeadless* headless) {
    if (g_mw) DestroyWindow(g_mw->wnd);   // one map window at a time
    if (!g_app || !g_app->dat_loaded) {
        MessageBoxW(owner, L"Open the .dat first (File > Open .dat...).", L"Map Window", MB_ICONINFORMATION);
        return;
    }
    register_classes();

    auto* m = new MapWin;
    m->mapFileId = mapFileId;
    m->datPath = g_app->data_gw2.file_info.file_path;
    m->gen = mapwin_next_generation();
    if (headless) m->headless = *headless;
    g_mw = m;

    // A client area that fits the toolbar, a 1280x720 surface and the status line.
    RECT r{0, 0, kSurfaceW, kToolbarH + kSurfaceH + kStatusH};
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    wchar_t title[96];
    swprintf(title, 96, L"Map Window - %u", mapFileId);
    // Not owned by the main window: an owned window always stays above its
    // owner and minimises with it, which would hide the preview a pick opens
    // there. The main window destroys it explicitly instead (close_map_window).
    m->wnd = CreateWindowExW(0, kWndClass, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, g_hinstance, nullptr);
    if (!m->wnd) {
        g_mw = nullptr;
        delete m;
        return;
    }

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, UINT_PTR id,
                  DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, m->wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    int x = 6;
    const int y = 5, h = 24;
    auto next = [&](int w) { int at = x; x += w + 4; return at; };
    m->bWalk = mk(L"BUTTON", L"Walk", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP, next(60), y, 60, h, ID_MW_WALK);
    m->bFly = mk(L"BUTTON", L"Fly", BS_AUTORADIOBUTTON | BS_PUSHLIKE, next(60), y, 60, h, ID_MW_FLY);
    x += 12;
    m->bTerrain = mk(L"BUTTON", L"Terrain", BS_AUTOCHECKBOX | WS_GROUP, next(70), y, 70, h, ID_MW_TERRAIN);
    m->bProps = mk(L"BUTTON", L"Props", BS_AUTOCHECKBOX, next(60), y, 60, h, ID_MW_PROPS);
    m->bColl = mk(L"BUTTON", L"Collision", BS_AUTOCHECKBOX, next(75), y, 75, h, ID_MW_COLLISION);
    m->bWater = mk(L"BUTTON", L"Water", BS_AUTOCHECKBOX, next(60), y, 60, h, ID_MW_WATER);
    m->bMaterials = mk(L"BUTTON", L"Materials", BS_AUTOCHECKBOX | BS_PUSHLIKE, next(75), y, 75, h, ID_MW_MATERIALS);
    m->bMotion = mk(L"BUTTON", L"Motion", BS_AUTOCHECKBOX | BS_PUSHLIKE, next(65), y, 65, h, ID_MW_MOTION);
    x += 12;
    m->bReset = mk(L"BUTTON", L"Reset", BS_PUSHBUTTON, next(70), y, 70, h, ID_MW_RESET);
    m->bWarn = mk(L"BUTTON", L"Warnings", BS_AUTOCHECKBOX | BS_PUSHLIKE, next(80), y, 80, h, ID_MW_WARNINGS);
    x += 12;
    mk(L"STATIC",
       L"WASD move, Q/E down/up, Shift fast, Ctrl slow, right-drag look, wheel speed, Tab walk/fly, Space jump, "
       L"click a prop to open it, F frame",
       SS_LEFT | SS_ENDELLIPSIS, x, y + 5, 2000, 16, 0);
    EnableWindow(m->bWalk, FALSE);   // until the collision index arrives
    SendMessageW(m->bFly, BM_SETCHECK, BST_CHECKED, 0);
    for (HWND b : {m->bTerrain, m->bProps, m->bWater, m->bMaterials, m->bMotion})
        SendMessageW(b, BM_SETCHECK, BST_CHECKED, 0);
    if (m->headless && m->headless->geometry) SendMessageW(m->bMaterials, BM_SETCHECK, BST_UNCHECKED, 0);

    m->surface = CreateWindowExW(0, kSurfaceClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, kToolbarH, kSurfaceW,
                                 kSurfaceH, m->wnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_MW_SURFACE)),
                                 g_hinstance, nullptr);
    m->warnEdit = mk(L"EDIT", L"(loading...)",
                     ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL, 0, 0, 10,
                     10, ID_MW_WARN_EDIT, WS_EX_CLIENTEDGE);
    ShowWindow(m->warnEdit, SW_HIDE);
    m->status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 6, 0, 10, 16, ID_MW_STATUS);
    layout(*m);

    m->viewOk = m->view.create(m->surface);
    if (!m->viewOk) {
        if (!m->headless)
            MessageBoxW(owner, L"Could not create a Direct3D 11 device for the Map window.", L"Map Window", MB_ICONERROR);
        if (m->headless) {
            m->error = "Direct3D device creation failed";
            headless_capture(*m);
        }
        DestroyWindow(m->wnd);
        return;
    }
    RECT rc;
    GetClientRect(m->surface, &rc);
    m->view.resize(rc.right, rc.bottom);
    apply_layer_buttons(*m);
    apply_shading_buttons(*m);
    if (m->headless && m->headless->fixedTime) m->view.set_time(m->headless->time);

    ShowWindow(m->wnd, SW_SHOW);
    SetFocus(m->surface);
    update_status(*m);
    SetTimer(m->wnd, TIMER_MW_FRAME, 16, nullptr);
    SetTimer(m->wnd, TIMER_MW_STATUS, 250, nullptr);
    mapwin_start_world(m->wnd, m->gen, m->datPath, mapFileId);
}

} // namespace castlemist::ui
