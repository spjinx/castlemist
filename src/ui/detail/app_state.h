/// @file
/// @brief The Win32 shell's shared state: window handles, control ids and the app model.
///
/// castlemist's UI is one window with one selected entry, so its state is one
/// struct. It is split across several translation units by *area of the window*
/// -- layout, preview, audio transport, video transport, content browser, file
/// commands, dialogs, window procedures -- and this header is the seam between
/// them.
///
/// @warning Not a public header -- internal to `src/ui/`.

#pragma once

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "castlemist/native/gw2dat.h"
#include "castlemist/ui/theme.h"

#include "castlemist/character/manifest.h"
#include "castlemist/db/index_db.h"
#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/chat_link.h"
#include "castlemist/format/content_map.h"
#include "castlemist/media/audio_player.h"
#include "castlemist/media/video_player.h"
#include "castlemist/render/d3d_renderer.h"
#include "castlemist/render/model_renderer.h"
#include "castlemist/ripper/armor_preview.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/native/BinaryParser.h"
#include "castlemist/ui/hexview.h"
#include "castlemist/ui/info_panel.h"
#include "castlemist/ui/mft_listview.h"
#include "castlemist/ui/splitter.h"
#include "castlemist/ui/struct_tree.h"
#include "castlemist/ui/texture_panel.h"

namespace castlemist::ui {

constexpr wchar_t kMainClassName[] = L"Gw2BrowserMain";
constexpr wchar_t kPreviewClassName[] = L"Gw2PreviewSurface";
constexpr wchar_t kModelClassName[] = L"Gw2ModelSurface";
/// The "Game 1:1" surface. A sibling of the model surface, not a replacement:
/// bgfx owns a device of its own and cannot share the one castlemist::render
/// created, so the two views get a window each and only one is shown at a time.
constexpr wchar_t kBgfxClassName[] = L"Gw2BgfxSurface";

constexpr int kSplitterThickness = 5;
constexpr int kSearchBarHeight = 118;  // id row + Type/Container row + Content row
constexpr int kMinPaneSize = 80;
constexpr int kTabHeight = 26;
constexpr int kToolbarHeight = 28;
constexpr int kStatusBarHeight = 26;

constexpr UINT_PTR ID_FILE_OPEN = 1001;
constexpr UINT_PTR ID_FILE_EXPORT_COMPRESSED = 1002;
constexpr UINT_PTR ID_FILE_EXPORT_DECOMPRESSED = 1003;
constexpr UINT_PTR ID_FILE_EXIT = 1004;
constexpr UINT_PTR ID_FILE_LOAD_TEMPLATE = 1005;
constexpr UINT_PTR ID_FILE_LOAD_KEYS = 1006;
constexpr UINT_PTR ID_FILE_OPEN_INDEX = 1007;
constexpr UINT_PTR ID_TOOLS_DECODE_LINK = 1008;
constexpr UINT_PTR ID_FILE_OPEN_LOOSE = 1009;  // 1008 is taken; WM_COMMAND ids must be unique
// View > Theme.
constexpr UINT_PTR ID_FILE_BUILD_INDEX = 1014;  // build an index DB from a .dat
constexpr UINT_PTR ID_VIEW_THEME_DARK   = 1010;
constexpr UINT_PTR ID_VIEW_THEME_LIGHT  = 1011;
constexpr UINT_PTR ID_VIEW_THEME_CUSTOM = 1012;
constexpr UINT_PTR ID_VIEW_THEME_ACCENT = 1013;
constexpr UINT_PTR ID_TOOLS_CHARACTER = 1019;      // Character Ripper (GW2 API) dialog
constexpr UINT_PTR ID_TOOLS_DOWNLOAD_NAMES = 1020; // fetch every game name into content_names.tsv
constexpr UINT_PTR ID_TOOLS_DECODE_TOKEN = 1015;   // token/filename-bytes decoder popup
constexpr UINT_PTR ID_FILE_EXPORT_GLTF_MODEL = 1016; // Export glTF... (single model, plain decoded textures)
constexpr UINT_PTR ID_FILE_EXPORT_GLTF_MAP = 1017;   // Export glTF... (whole map scene)
constexpr UINT_PTR ID_FILE_EXPORT_GLTF_MODEL_ATLAS = 1018; // Export glTF... (single model, baked to a fresh UV atlas)
constexpr UINT_PTR ID_FILE_EXPORT_VRCHAT_MODEL = 2185;     // Export for VRChat (Model)... (folder: glb, fbx, blend, Textures, materials.json)
constexpr UINT_PTR ID_FILE_EXPORT_SKYBOX_MAP = 2186;       // Export Skybox (Map)... (folder: sky.json, <mode>/baked, <mode>/skybox)
// Chat-link decoder popup controls.
constexpr int ID_CL_INPUT = 2070;
constexpr UINT_PTR ID_CL_DECODE = 2071;
constexpr int ID_CL_OUTPUT = 2072;
constexpr UINT_PTR ID_CL_SEARCH_BASE = 2073;
constexpr UINT_PTR ID_CL_SEARCH_FILE = 2074;
constexpr UINT_PTR ID_CL_CLOSE = 2075;
constexpr UINT_PTR ID_CL_RESOLVE = 2076;
constexpr UINT_PTR ID_CL_OPEN_ICON = 2077;
constexpr UINT_PTR ID_CL_OPEN_MODEL = 2078;
constexpr UINT_PTR ID_CL_REBUILD = 2080;

// Character Ripper dialog (character_dialog.cpp) and its key manager (character_keys_dialog.cpp).
constexpr UINT_PTR ID_CH_KEY_COMBO = 2150;
constexpr UINT_PTR ID_CH_MANAGE = 2151;
constexpr UINT_PTR ID_CH_FETCH = 2152;
constexpr UINT_PTR ID_CH_CHAR_LIST = 2153;
constexpr UINT_PTR ID_CH_TAB_COMBO = 2154;
constexpr UINT_PTR ID_CH_TABLE = 2155;
constexpr UINT_PTR ID_CH_OPEN_MODEL = 2156;
constexpr UINT_PTR ID_CH_SAVE = 2157;
constexpr UINT_PTR ID_CH_CLOSE = 2158;
constexpr UINT_PTR ID_CH_BUILD_MAP = 2159;
constexpr UINT_PTR ID_CK_LIST = 2160;
constexpr UINT_PTR ID_CK_NAME = 2161;
constexpr UINT_PTR ID_CK_KEY = 2162;
constexpr UINT_PTR ID_CK_SHOW = 2163;
constexpr UINT_PTR ID_CK_SAVE = 2164;
constexpr UINT_PTR ID_CK_RENAME = 2165;
constexpr UINT_PTR ID_CK_REMOVE = 2166;
constexpr UINT_PTR ID_CK_CLOSE = 2167;
constexpr UINT_PTR ID_CH_EXPORT = 2168;
constexpr UINT_PTR ID_CH_ASSEMBLE = 2169;
constexpr UINT_PTR ID_CH_COMBINE = 2170;  // "Combine into one file" checkbox
// The Look row: face / hair style combos, skin / hair / hair 2 colour swatches.
constexpr UINT_PTR ID_CH_FACE = 2171;
constexpr UINT_PTR ID_CH_HAIR = 2172;
constexpr UINT_PTR ID_CH_SKIN_COLOR = 2173;
constexpr UINT_PTR ID_CH_HAIR_COLOR = 2174;
constexpr UINT_PTR ID_CH_HAIR_COLOR2 = 2175;
constexpr UINT_PTR ID_CH_SAVE_LOOK = 2176;
constexpr UINT_PTR ID_CH_EDIT_LOOK = 2177;  // opens the Look dialog
constexpr UINT_PTR ID_CH_VRCHAT = 2178;     // Export for VRChat...
constexpr int ID_LISTVIEW = 2001;
constexpr int ID_HEX_BEFORE = 2002;
constexpr int ID_HEX_AFTER = 2003;
constexpr int ID_STRUCT_TREE = 2110; // "Structure" tab: JSON-template-driven parsed field tree
constexpr int ID_STRUCT_CONTAINER_COMBO = 2130; // manual container/chunk-schema override for the tree above
constexpr int ID_INFO_PANEL = 2004;
constexpr int ID_TAB = 2005;
constexpr int ID_SPLIT_LIST_MIDDLE = 2010;
constexpr int ID_SPLIT_MIDDLE_INFO = 2011;
constexpr int ID_SPLIT_CONTENT = 2012;    // cntc: browser block | preview
constexpr int ID_SPLIT_CONTENT_H = 2013;  // cntc: top tables | assets table
constexpr int ID_SEARCH_EDIT = 2020;
constexpr int ID_SEARCH_FILEID_CHECK = 2021;
constexpr UINT_PTR ID_SEARCH_BUTTON = 2022;
constexpr UINT_PTR ID_CLEAR_BUTTON = 2023;
constexpr int ID_FILTER_TYPE = 2024;
constexpr int ID_FILTER_CONTAINER = 2025;
constexpr int ID_FILTER_CONTENT = 2026;

/// Separates a filter combo's raw db value (a bare `entries.type`/`entries.container`
/// value, e.g. "MODL") from the friendly name index_ui.cpp appends after it (e.g.
/// "MODL — Model"), so file_ops.cpp's combo_sel() can strip it back off before
/// the value goes into a SQL WHERE clause. Never appears in a raw type/container
/// value itself (those are plain ASCII fourccs/words with no dash).
inline constexpr const wchar_t* kFilterLabelSep = L" — ";

/// @brief One entry in the index-mode "Content" filter combo.
///
/// Maps a human label to a ::castlemist::db::ContentFilter. Kept here so the
/// combo that displays them and the query that applies them cannot drift: the
/// combo is populated by walking this table, and the selected index reads back
/// out of it.
struct ContentFilterChoice {
    const wchar_t* label;
    const char* magics;         ///< comma-separated entries.magic values, "" = any
    const char* require_chunk;  ///< chunk fourcc the entry must carry
    const char* exclude_chunk;  ///< chunk fourcc the entry must NOT carry
};

/// Index 0 must stay the no-op so "(any content)" is the default selection.
///
/// The MODL pair is the reason this exists: 198330 entries declare the MODL
/// container but only 183166 carry a GEOM chunk, so "container = MODL" alone
/// mixes 15164 animation-only files in with the actual meshes. Note "DDS "
/// carries a trailing pad byte in the index -- that is the stored magic, not a
/// typo.
/// The ArenaNet texture family. `gw2_atex.hpp` accepts ATEX/ATTX/ATEC/ATEP/ATEU/
/// ATET; the archive additionally carries CTEX, and the C-prefixed variants pair
/// with the A-prefixed ones. Listed in full rather than trimmed to what one
/// archive happens to hold, so the filter still works on a different .dat.
#define CM_TEXTURE_MAGICS "ATEX,ATTX,ATEC,ATEP,ATEU,ATET,CTEX,CTTX,CTEC,CTEP,CTEU,CTET"

inline constexpr ContentFilterChoice kContentFilters[] = {
    {L"(any content)",                  "",                  "",     ""},
    // Counts below are from the shipped archive, as a sanity check on the predicate.
    {L"3D model - has GEOM mesh",       "",                  "GEOM", ""},        // 183166
    // NOT just "no GEOM" -- that matches every texture and sound too (624989).
    // Anim-only models are the ones that carry ANIM but no mesh.
    {L"Anim only - ANIM, no mesh",      "",                  "ANIM", "GEOM"},    // 15164
    {L"Rigged - has SKEL",              "",                  "SKEL", ""},        // 183166
    {L"Has collision - COLL",           "",                  "COLL", ""},        // 111626
    {L"Has properties - PRPS",          "",                  "PRPS", ""},
    {L"Texture - any ArenaNet",         CM_TEXTURE_MAGICS,   "",     ""},        // 425437
    {L"Texture - ATEX",                 "ATEX",              "",     ""},        // 334887
    {L"Texture - ATEU",                 "ATEU",              "",     ""},        // 46439
    {L"Texture - ATEP",                 "ATEP",              "",     ""},        // 44045
    {L"Texture - ATEC",                 "ATEC",              "",     ""},
    {L"Texture - ATTX",                 "ATTX",              "",     ""},
    {L"Texture - ATET",                 "ATET",              "",     ""},
    {L"Texture - CTEX",                 "CTEX",              "",     ""},        // 66
    {L"Texture - DDS",                  "DDS ",              "",     ""},        // 17727 (padded magic)
    {L"Cinematic scene - CSCN",         "",                  "CSCN", ""},        // 1446
    {L"Material shaders - BGFX",        "",                  "BGFX", ""},        // 2817
    {L"Map props - prp2",               "",                  "prp2", ""},        // 284
    // The fourcc really is "trn." with a trailing dot, like "env." and "msn.".
    {L"Map terrain - trn.",             "",                  "trn.", ""},        // 284
};
constexpr UINT_PTR ID_ZOOM_IN = 2030;
constexpr UINT_PTR ID_ZOOM_OUT = 2031;
constexpr UINT_PTR ID_ROTATE = 2032;
constexpr UINT_PTR ID_FIT = 2033;
constexpr UINT_PTR ID_MODE_FULL = 2034;
constexpr UINT_PTR ID_MODE_PLAIN = 2035;
constexpr UINT_PTR ID_MODE_WIRE = 2036;
constexpr UINT_PTR ID_MODEL_RESET = 2037;
constexpr UINT_PTR ID_MODE_SKEL = 2038;
constexpr UINT_PTR ID_MODE_SHADER = 2039;
/// "Game 1:1" -- switches the model pane to the bgfx surface (a second,
/// independent view; the Direct3D 11 renderer keeps its own modes above).
constexpr UINT_PTR ID_MODE_GW2BGFX = 2131;
constexpr UINT_PTR ID_ANIM_COMBO = 2050;
constexpr UINT_PTR ID_ANIM_PLAY = 2051;
constexpr UINT_PTR ID_LAYER_PROP = 2052;
constexpr UINT_PTR ID_LAYER_ZONE = 2053;
constexpr UINT_PTR ID_LAYER_COLL = 2054;
/// Terrain had no toggle -- it was forced on -- so the ground plane could not be
/// hidden to look at what sits under it.
constexpr UINT_PTR ID_LAYER_TERRAIN = 2132;
/// "Fly": the mesh-only free-camera map view (RenderMode::MapFly).
constexpr UINT_PTR ID_MODE_FLY = 2133;
/// "Map": with a map and a model both loaded, picks which owns the surface.
constexpr UINT_PTR ID_SHOW_MAP = 2134;
constexpr UINT_PTR ID_LAYER_WATER = 2135;
constexpr UINT_PTR ID_LAYER_NAVMESH = 2136;
constexpr UINT_PTR ID_UV_MAP_BTN = 2137;  // single-model: opens the UV map viewer popup
constexpr UINT_PTR ID_UV_CLOSE = 2138;    // UV map viewer popup's own "Close" button
constexpr UINT_PTR ID_BAKESEL_LIST = 2139;    // atlas-bake material selection popup: the checked list
constexpr UINT_PTR ID_BAKESEL_ALL = 2140;     // "Select All" button
constexpr UINT_PTR ID_BAKESEL_NONE = 2141;    // "Select None" button
constexpr UINT_PTR ID_BAKESEL_OK = 2142;      // "Bake Selected" button
constexpr UINT_PTR ID_BAKESEL_CANCEL = 2143;  // cancels the export entirely
constexpr UINT_PTR ID_TEX_FULLRES = 2055;
constexpr UINT_PTR ID_ARMOR_DYES = 2195;    // "Dyes": the armor dye channels window
constexpr UINT_PTR ID_AUDIO_PLAY = 2056;
constexpr UINT_PTR ID_AUDIO_STOP = 2057;
constexpr UINT_PTR ID_AUDIO_COMBO = 2058;
constexpr UINT_PTR ID_CONTENT_LIST = 2059;   // master: content types
constexpr UINT_PTR ID_CONTENT_CHILD = 2079;  // child: entries of the selected type
constexpr UINT_PTR ID_LIGHT_PREPASS = 2060;
constexpr UINT_PTR ID_ALPHA_TOGGLE = 2061;
// Texture channel view: RGB, R, G, B, A (consecutive; castlemist::gfx::set_channel order).
constexpr UINT_PTR ID_CHANNEL_RGB = 2180;
constexpr UINT_PTR ID_CHANNEL_A = ID_CHANNEL_RGB + 4;
constexpr UINT_PTR ID_SUBMESH_COMBO = 2062;
constexpr UINT_PTR ID_LOD_COMBO = 2063;
constexpr UINT_PTR ID_TEX_REDUCED = 2064;
constexpr UINT_PTR ID_EFFECTS_TOGGLE = 2065;
constexpr UINT_PTR ID_LIGHT_SLIDER = 2081;   // model light intensity trackbar
constexpr UINT_PTR ID_LIGHT_ANGLE = 2090;    // model headlight angle trackbar
constexpr UINT_PTR ID_LIGHT_FOLLOW = 2091;   // "follow camera" (headlight) toggle
constexpr UINT_PTR ID_LIGHT_GW2RIG = 2092;   // "GW2 rig" -- the game's own preview light rig
constexpr UINT_PTR ID_CONTENT_ASSET_LIST = 2082; // cntc entry asset selector (sortable table)
// Blender-style transform gizmo controls (single-model surface).
constexpr UINT_PTR ID_GIZMO_MOVE = 2083;
constexpr UINT_PTR ID_GIZMO_ROTATE = 2084;
constexpr UINT_PTR ID_GIZMO_SCALE = 2085;
constexpr UINT_PTR ID_GIZMO_GRID = 2086;
constexpr UINT_PTR ID_GIZMO_RESET = 2087;
constexpr UINT_PTR ID_TEX_PANEL = 2103;     // single-model: "Textures" toggle for the panel below
constexpr int ID_TEX_INFO = 2104;           // the per-submesh texture panel itself (bottom right)
constexpr int ID_SPLIT_INFO_TEX = 2105;     // horizontal: info panel | texture panel
// Token / filename-bytes decoder popup controls.
constexpr int ID_TD_SHADER_IN = 2120;       // shader/material token32 (hex) input
constexpr UINT_PTR ID_TD_SHADER_DECODE = 2121;  // token32 -> name
constexpr UINT_PTR ID_TD_SHADER_ENCODE = 2122;  // name -> token32
constexpr int ID_TD_BONE_IN = 2123;         // bone name input
constexpr UINT_PTR ID_TD_BONE_ENCODE = 2124;    // name -> token64
constexpr int ID_TD_FID_IN = 2125;          // filename-record hex bytes input
constexpr UINT_PTR ID_TD_FID_DECODE = 2126;     // bytes -> fileId/subId
constexpr int ID_TD_OUTPUT = 2127;          // shared read-only log
constexpr UINT_PTR ID_TD_CLOSE = 2128;
constexpr UINT_PTR ID_MAP_PREVIEW = 2088;   // map: toggle the picked-prop inset preview
constexpr UINT_PTR ID_CLOTH_TOGGLE = 2089;  // single-model: live cloth simulation on/off
constexpr UINT_PTR ID_AUDIO_SEEK = 2092;    // audio playback position / seek trackbar
constexpr UINT_PTR ID_STRS_LIST = 2093;     // strs string-table report table
// Bink video player controls (Preview surface for KB2*/BIK* cinematics).
constexpr UINT_PTR ID_VIDEO_PLAY = 2094;    // Play / Pause toggle
constexpr UINT_PTR ID_VIDEO_STOP = 2095;    // stop + rewind to frame 1
constexpr UINT_PTR ID_VIDEO_LOOP = 2096;    // loop toggle
constexpr UINT_PTR ID_VIDEO_MUTE = 2097;    // mute toggle
constexpr UINT_PTR ID_VIDEO_SEEK = 2098;    // position / seek trackbar (permille)
constexpr UINT_PTR ID_VIDEO_VOLUME = 2099;  // volume trackbar (0..100)
constexpr UINT_PTR ID_VIDEO_TRACK = 2100;   // audio-track selector (multi-track Binks)
constexpr UINT_PTR ID_VIDEO_SUBS = 2101;    // subtitle toggle (searches the CINP scripts)
constexpr UINT_PTR ID_VIDEO_CLIP = 2102;    // which movie of a CINP cinematic to play
constexpr UINT_PTR TIMER_ANIM = 1;
/// ~60 Hz pump for the fly view: integrates WASD movement and repaints. Runs
/// only while the fly view owns the surface.
constexpr UINT_PTR TIMER_FLY = 4;
constexpr UINT_PTR TIMER_AUDIO = 2;         // ~10 Hz refresh of the audio seek bar / time
constexpr UINT_PTR TIMER_VIDEO = 3;         // video frame pump (see on_video_tick)
constexpr int kAudioSeekMax = 1000;         // seek trackbar range (permille of duration)
constexpr int kVideoSeekMax = 1000;         // video seek trackbar range (permille of frames)
constexpr int ID_STATUS_LABEL = 2040;
constexpr int ID_PROGRESS = 2041;

constexpr UINT WM_APP_EXTRACT_DONE = WM_APP + 1;
constexpr UINT WM_APP_CMAP_DONE = WM_APP + 2;
/// Posted from the index-build worker; wparam = entries done, lparam = total.
/// Progress has to cross threads by message: the builder runs off the UI thread
/// and must never touch a window handle itself.
constexpr UINT WM_APP_INDEX_PROGRESS = WM_APP + 3;
/// Posted when the build finishes; wparam = 1 on success, 0 on failure/cancel.
constexpr UINT WM_APP_INDEX_DONE = WM_APP + 4;
/// Posted when a background glTF export finishes; result is in g_gltf_export_result.
constexpr UINT WM_APP_GLTF_EXPORT_DONE = WM_APP + 5;
// Character Ripper workers -> dialog; lParam owns a heap result the dialog deletes.
constexpr UINT WM_APP_CHAR_NAMES_DONE = WM_APP + 6;
constexpr UINT WM_APP_CHAR_FETCH_DONE = WM_APP + 7;
constexpr UINT WM_APP_CHAR_EXPORT_DONE = WM_APP + 8;
constexpr UINT WM_APP_CHAR_ASSEMBLE_DONE = WM_APP + 9;
constexpr UINT WM_APP_CHAR_VRCHAT_DONE = WM_APP + 11;  // (+10, +12: look_dialog)
/// Posted to the main window when game names for content objects arrive.
constexpr UINT WM_APP_CONTENT_NAMES_DONE = WM_APP + 13;
/// Download all game names: wparam = done, lparam = total; then done (wparam = ok).
constexpr UINT WM_APP_NAMES_PROGRESS = WM_APP + 14;
constexpr UINT WM_APP_NAMES_BULK_DONE = WM_APP + 15;
/// "Export for VRChat (Model)" finished (lparam = heap VrchatModelReport*, owned by the handler).
constexpr UINT WM_APP_VRCHAT_MODEL_DONE = WM_APP + 16;
/// "Export Skybox (Map)" finished (lparam = heap SkyExportReport*, owned by the handler).
constexpr UINT WM_APP_SKYBOX_EXPORT_DONE = WM_APP + 17;

enum class MiddleTab { Compressed = 0, Decompressed = 1, Structure = 2, Preview = 3 };

// Result of a background extract_entry() call, handed back to the UI thread
// via PostMessageW (WPARAM = generation, LPARAM = heap pointer to this,
// ownership transferred to whoever handles WM_APP_EXTRACT_DONE).
struct ExtractResult {
    uint64_t generation = 0;
    uint32_t mft_index = 0;
    bool success = false;
    std::string error_message;
    ExtractedEntry entry;
};

inline HINSTANCE g_hinstance = nullptr;
inline HMENU g_file_menu = nullptr;
inline HMENU g_theme_menu = nullptr;  // View > Theme, for the radio mark

struct AppState {
    Gw2Dat data_gw2;
    ExtractedEntry current_entry;
    uint32_t current_mft_index = 0;
    /// Armor dye channels the viewer bakes into armor's rebuilt atlas. Kept
    /// across models, like a wardrobe's dye pick.
    std::array<castlemist::ripper::DyeChoice, 4> armor_dyes{};
    /// Dye channels of models dyed in the shader (mounts: ripper/shader_dye.h).
    /// Colour id 0 leaves a channel as authored, which is where every channel
    /// starts. Kept across models, apart from the armor picks.
    std::array<castlemist::ripper::DyeChoice, 4> shader_dyes = {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}};
    /// The current model as extracted, before its atlas stand-in or shader dyes
    /// were applied (null when it has no dye channels): re-dyeing rebuilds from it.
    std::shared_ptr<ModelPreview> armor_pristine;
    std::array<bool, 4> armor_channels{};  // which channels the current piece has
    /// The current model dyes in the shader (shader_dyes), not in the armor atlas (armor_dyes).
    bool dye_in_shader = false;
    bool dat_loaded = false;

    // Index-DB navigation (Stage 2). When an index is loaded, the list gains
    // Type/Container columns + filters. index_meta maps base_id -> interned
    // (typeIdx<<16 | contIdx) so 800k rows cost ~3MB, not per-cell SQL.
    bool index_loaded = false;
    std::vector<std::string> idx_type_names, idx_cont_names;
    std::unordered_map<uint32_t, uint32_t> index_meta;
    // Real decompressed size per base_id, straight from the index. base_ids are
    // dense (1..N with density 1.000 on a retail dat), so a flat array indexed by
    // base_id is both the smallest and fastest choice (~6MB for 808k entries) --
    // the list asks for this on every visible-row repaint.
    std::vector<uint64_t> index_usize;
    bool has_loaded_entry = false; // current_entry reflects a *completed* extraction, safe to export

    // Bumped on every new selection (and on opening a new archive); a
    // background result is only applied if its snapshot still matches this
    // when it comes back -- anything older is silently discarded, which is
    // what makes "select something else before the old one finishes" work.
    uint64_t request_generation = 0;

    HWND hwnd_main = nullptr;
    HWND hwnd_status_label = nullptr;
    HWND hwnd_progress = nullptr;
    HWND hwnd_search_edit = nullptr;
    HWND hwnd_filter_type = nullptr;       // index-mode type filter combo
    HWND hwnd_filter_container = nullptr;  // index-mode container filter combo
    HWND hwnd_filter_content = nullptr;    // index-mode "what does it contain" filter combo
    HWND hwnd_search_fileid_check = nullptr;
    HWND hwnd_search_button = nullptr;
    HWND hwnd_clear_button = nullptr;
    HWND hwnd_list = nullptr;
    HWND hwnd_tab = nullptr;
    HWND hwnd_preview = nullptr;      // image D3D surface (gw2gfx)
    HWND hwnd_model = nullptr;        // model D3D surface (gw2m3d)
    HWND hwnd_text_preview = nullptr; // text read-only edit
    HWND hwnd_strs_list = nullptr;    // strs string table as a sortable report table
    HWND hwnd_content_list = nullptr;  // master: cntc content TYPES (kind == Content)
    HWND hwnd_content_child = nullptr; // child: the entries (objects) of the selected type
    int content_type_sel = -1;         // selected master type row (into content_types)
    int content_obj_sel = -1;          // selected entry -> index into content_objects
    // Distinct content types present in current_entry.content_objects, each with
    // its object count. Master list row i <-> content_types[i]. Rebuilt per entry.
    std::vector<std::pair<uint32_t, uint32_t>> content_types;
    // Child list row -> index into content_objects (the entries of the selected type).
    std::vector<int> content_child_objidx;
    HWND hwnd_content_asset_list = nullptr; // asset selector table for the selected entry
    // Per-table sort state (0 = types, 1 = entries, 2 = assets); col < 0 = unsorted.
    int content_sort_col[3] = {-1, -1, -1};
    bool content_sort_asc[3] = {true, true, true};
    ExtractedEntry content_sub;       // the asset currently selected in the content list
    bool content_sub_loaded = false;  // content_sub holds a valid loaded asset
    HWND hwnd_info = nullptr;
    // Bottom of the right-hand column: the per-submesh texture panel, with its own
    // horizontal splitter against the info panel above it. Only claims space while
    // a model preview is up and the "Textures" toggle is on.
    HWND hwnd_tex_info = nullptr;
    HWND hwnd_split_info_tex = nullptr;
    double tex_panel_ratio = 0.42;   // fraction of the right column the panel gets
    HWND hwnd_hex_before = nullptr;
    HWND hwnd_hex_after = nullptr;
    // "Structure" tab: the current entry's decompressed bytes walked against
    // the loaded JSON struct template and shown as a category tree (chunk ->
    // fields), immediately after the two hex panels in tab order.
    HWND hwnd_struct_tree = nullptr;
    // "Container:" combo above the Structure tree is a navigator, not a type
    // filter: it lists every top-level entry actually present in the CURRENT
    // entry's own parsed packfile (the PF header row, plus one row per real
    // chunk -- fourcc, version, and its resolved schema name -- see
    // populate_struct_container_combo()), sourced straight from the last
    // ParsedNode tree BinaryParser produced for these bytes. Picking one jumps
    // the tree view to that node (struct_tree::select_chunk_by_offset) instead
    // of reparsing anything, since the tree already holds every chunk this
    // file actually has. Item 0 is always "(select a chunk)".
    // struct_root keeps that last-parsed tree alive so the combo can be
    // rebuilt (e.g. after switching entries) without re-running BinaryParser;
    // it mirrors what struct_tree.cpp itself is already holding onto for the
    // visible tree, just reachable from app state too.
    ParsedNodePtr struct_root;
    HWND hwnd_struct_container_label = nullptr;
    HWND hwnd_struct_container_combo = nullptr;
    // True when the struct tree is stale for the currently selected entry --
    // set whenever a new entry is selected, cleared once populate_struct_tree()
    // actually runs. Keeps the Structure tab lazy: the struct-template JSON and
    // the per-entry binary walk only happen the first time the tab is actually
    // shown for a given entry, not on every entry click regardless of which
    // tab the user is looking at.
    bool struct_tree_dirty = false;
    HWND hwnd_split_list_middle = nullptr;
    HWND hwnd_split_middle_info = nullptr;
    HWND hwnd_zoom_in = nullptr;
    HWND hwnd_zoom_out = nullptr;
    HWND hwnd_rotate = nullptr;
    HWND hwnd_fit = nullptr;
    HWND hwnd_alpha = nullptr;
    HWND hwnd_channel[5] = {};  // RGB, R, G, B, A
    /// The "Game 1:1" bgfx surface and its toolbar toggle. Both stay null when
    /// the build has no bgfx (castlemist::gw2bgfxview::available() == false).
    HWND hwnd_model_bgfx = nullptr;
    HWND hwnd_mode_gw2bgfx = nullptr;
    HWND hwnd_mode_full = nullptr;
    HWND hwnd_mode_plain = nullptr;
    HWND hwnd_mode_wire = nullptr;
    HWND hwnd_mode_shader = nullptr;
    HWND hwnd_model_reset = nullptr;
    HWND hwnd_skel_toggle = nullptr;
    HWND hwnd_anim_combo = nullptr;
    HWND hwnd_anim_play = nullptr;
    HWND hwnd_tex_fullres = nullptr;
    HWND hwnd_armor_dyes = nullptr;   // "Dyes" button, shown for armor with a rebuilt atlas
    HWND hwnd_dye_wnd = nullptr;      // the Dyes window, while open
    HWND hwnd_light_toggle = nullptr;
    HWND hwnd_effects_toggle = nullptr;
    HWND hwnd_cloth_toggle = nullptr;
    HWND hwnd_light_label = nullptr;   // "Light" caption for the intensity slider
    HWND hwnd_light_slider = nullptr;  // model light-intensity trackbar (Full/Plain)
    HWND hwnd_light_angle = nullptr;   // headlight angle trackbar (behind-cam .. grazing)
    HWND hwnd_light_follow = nullptr;  // "Follow cam" headlight toggle
    HWND hwnd_light_gw2rig = nullptr;  // "GW2 rig" -- game's own preview lighting
    HWND hwnd_submesh_combo = nullptr; // LOD/texture target: "All submeshes" + each submesh
    HWND hwnd_lod_combo = nullptr;     // LOD level selector
    HWND hwnd_tex_reduced = nullptr;   // reduced (half-res) texture toggle
    HWND hwnd_uv_map_btn = nullptr;    // opens the UV map viewer popup
    HWND hwnd_audio_play = nullptr;
    HWND hwnd_audio_stop = nullptr;
    HWND hwnd_audio_combo = nullptr; // sound selector for multi-sound banks
    HWND hwnd_audio_seek = nullptr;  // playback position / seek trackbar (0..1000 permille)
    HWND hwnd_audio_time = nullptr;  // "m:ss / m:ss" position/duration label
    bool audio_seek_dragging = false; // user is scrubbing the seek bar (pause auto-updates)
    // Format/duration of the clip currently SELECTED (probed on every selection
    // change, before any playback). The seek bar's total time comes from here, so
    // switching sounds in a bank immediately shows that sound's real duration
    // instead of whatever was last played. Cached because probe() fully decodes.
    castlemist::snd::ClipInfo audio_sel_info;
    // --- Bink video player (Preview surface) ---
    HWND hwnd_video_play = nullptr;   // Play / Pause
    HWND hwnd_video_stop = nullptr;
    HWND hwnd_video_loop = nullptr;
    HWND hwnd_video_mute = nullptr;
    HWND hwnd_video_seek = nullptr;   // position trackbar (0..1000 permille of frames)
    HWND hwnd_video_volume = nullptr; // volume trackbar (0..100)
    HWND hwnd_video_time = nullptr;   // "m:ss / m:ss  (frame N/M)" readout
    HWND hwnd_video_track = nullptr;  // audio-track combo (only for multi-track Binks)
    HWND hwnd_video_subs = nullptr;   // "Subs" toggle
    HWND hwnd_video_subtitle = nullptr; // overlay label across the bottom of the video
    // Dialogue pulled from the CINP cinematic that drives the current movie.
    std::vector<SubtitleLine> video_subs;
    bool     video_subs_searched = false; // the CINP scan already ran for this entry
    uint32_t video_subs_cinp = 0;         // CINP baseId the lines came from (0 = none)
    // Off until asked for: turning it on triggers the CINP scan, which costs a
    // couple of seconds, so it must be an explicit choice rather than a tax on
    // every video preview.
    bool     video_subs_on = false;
    int      video_sub_shown = -1;        // index of the line currently on screen
    HWND     hwnd_video_clip = nullptr;   // CINP: which referenced movie is playing
    // A CINP entry's own bytes are the script; the movie it plays is a separate
    // dat entry, loaded here and kept alive for as long as gw2vid reads it.
    std::vector<uint8_t> cinp_video_bytes;
    int      cinp_video_sel = -1;
    bool video_seek_dragging = false; // scrubbing: suspend automatic seek-bar updates
    HWND hwnd_layer_prop = nullptr;
    HWND hwnd_layer_zone = nullptr;
    HWND hwnd_layer_coll = nullptr;
    HWND hwnd_layer_terrain = nullptr;
    HWND hwnd_layer_water = nullptr;
    HWND hwnd_layer_navmesh = nullptr;
    HWND hwnd_map_preview = nullptr; // map: toggle the picked-prop inset preview
    HWND hwnd_show_map = nullptr;    // "Map": surface shows the map, not the model
    bool map_zone_loaded = false; // whether the zone layer has been lazily loaded

    // --- mesh-only fly view (RenderMode::MapFly) ---
    HWND hwnd_mode_fly = nullptr;
    HWND hwnd_fly_hud = nullptr;   // overlay label: position, speed, draws, ms
    bool fly_looking = false;      // a mouse-look drag is in progress
    POINT fly_look_last{};
    LARGE_INTEGER fly_qpc_last{};  // for the movement timestep
    bool fly_qpc_init = false;

    // Blender-style gizmo controls + transform readout (single-model surface).
    HWND hwnd_gizmo_move = nullptr;
    HWND hwnd_gizmo_rotate = nullptr;
    HWND hwnd_gizmo_scale = nullptr;
    HWND hwnd_gizmo_grid = nullptr;
    HWND hwnd_gizmo_reset = nullptr;
    HWND hwnd_tex_panel = nullptr;     // "Textures" toggle for the bottom-right texture panel
    HWND hwnd_gizmo_readout = nullptr; // overlay label: Loc/Rot/Scale values
    bool gizmo_dragging = false;       // a gizmo handle is being dragged
    int  gizmo_hover_axis = -1;        // last hovered handle (for highlight)

    // Layout ratios (0..1) of available space; scale sanely on window resize.
    double list_width_ratio = 0.22;
    double info_width_ratio = 0.18;
    // cntc content-browser sizing (resizable via its own splitters): total width of
    // the Types|Entries|Assets block, and the fraction of that block's height the
    // Assets table gets (Types/Entries share the rest).
    int content_browser_w = 416;
    double content_assets_ratio = 0.35;
    HWND hwnd_split_content = nullptr;   // vertical: browser block | preview surface
    HWND hwnd_split_content_h = nullptr; // horizontal: Types/Entries | Assets

    // Preview zoom/pan/rotation (mirrors gw2gfx's internal state so drag math
    // has something to read back without adding renderer getters).
    bool preview_dragging = false;
    POINT preview_drag_last{};
    float preview_zoom = 1.0f;
    float preview_pan_x = 0.0f;
    float preview_pan_y = 0.0f;
    int preview_rotation_quarters = 0;

    /// "Game 1:1" view: on = the bgfx surface is shown in place of the D3D11
    /// one. Its own orbit-drag state, since the two surfaces are separate
    /// windows with separate cameras.
    bool bgfx_view_active = false;
    bool bgfx_dragging = false;
    POINT bgfx_drag_last{};
    /// Set when the current entry has been handed to the bgfx view, so the
    /// surface is not reloaded on every repaint.
    bool bgfx_model_loaded = false;

    // Model orbit-drag state (mirrors the image drag state above).
    bool model_dragging = false;
    POINT model_drag_last{};
    POINT model_down{};        // where the left button went down (click-vs-drag test)
    bool map_pick_enabled = true; // map: a click (no drag) picks a prop into the inset preview
    castlemist::render::RenderMode model_mode = castlemist::render::RenderMode::Full;
    bool show_skeleton = false; // bind-pose skeleton overlay toggle (persists across models)
    bool show_tex_panel = false; // per-submesh texture strip toggle (persists across models)
};

inline AppState* g_app = nullptr;

inline HFONT g_ui_font = nullptr;   // Segoe UI, applied to every control
inline HFONT g_ui_font_bold = nullptr;

// ---------------------------------------------------------------------------
// Cross-file entry points, grouped by the file that defines them.
// ---------------------------------------------------------------------------

// ---- window_proc.cpp
/// @brief Put the radio mark on the entry matching g_theme_mode.
void sync_theme_menu();
void ensure_ui_fonts();
BOOL CALLBACK apply_font_cb(HWND child, LPARAM font);

// ---- listview_util.cpp -- report-view ListView helpers
void lv_add_col(HWND lv, int i, const wchar_t* text, int width);
int lv_add_row(HWND lv, LPARAM param, const wchar_t* col0,
               const wchar_t* col1 = nullptr, const wchar_t* col2 = nullptr,
               const wchar_t* col3 = nullptr, const wchar_t* col4 = nullptr);
LPARAM lv_selected_param(HWND lv);

// ---- layout.cpp -- where every child window goes
void layout_children(int client_w, int client_h);
void relayout();

// ---- view_controls.cpp -- toolbar commands and the model-view controls
void set_export_enabled(bool enabled);
void show_loading(bool loading, uint32_t mft_index);
void zoom_by(float factor);
void rotate_90();
void fit_view();
void ensure_map_game_materials();
void set_model_mode(castlemist::render::RenderMode mode);
void reset_model_view();
void update_gizmo_readout();
/// True when the mesh-only fly view currently owns the model surface.
bool fly_view_active();
/// Starts/stops TIMER_FLY to match fly_view_active(), and shows/hides the HUD.
void update_fly_timer();
/// Refreshes the fly HUD overlay (position, speed, draw calls, frame time).
void update_fly_hud();
void set_gizmo_mode_ui(castlemist::render::GizmoMode m);
int lod_target_submesh();
void refresh_lod_controls();
void populate_lod_controls();

// ---- audio_ui.cpp -- the audio transport and the strs table
ExtractedEntry& active_audio_entry();
int audio_selected_index();
size_t populate_strs_table(const std::vector<uint8_t>& bytes, long long base);
void update_audio_info(int sel);
void update_audio_seek_ui(bool reset);

// ---- video_ui.cpp -- the Bink transport, subtitles and cinematic clips
void update_video_ui(bool reset);
void layout_video_subtitle();
void present_video_frame();
void stop_video_stream();
void stop_video();
void populate_video_tracks();
void populate_video_clips();
void search_video_subtitles(uint32_t mft_index);
std::wstring start_video(const ExtractedEntry& e);
std::wstring cinematic_info_text(const ExtractedEntry& e, const std::wstring& error);
std::wstring start_cinp_video(int index);
std::wstring video_info_text(const VideoMeta& v, const std::wstring& error);

// ---- content_browser.cpp -- the cntc types/entries/assets drill-down
void render_content_sub();
std::wstring content_asset_kind(uint32_t fid);
void load_content_asset_fid(uint32_t fid);
void populate_content_master();
void on_content_master_select(int idx);
const std::vector<uint32_t>& selected_entry_assets();
void on_content_child_select(int oi);
void on_content_asset_select();
std::wstring content_type_label(uint32_t type);
int CALLBACK content_lv_compare(LPARAM l1, LPARAM l2, LPARAM lpctx);
void content_sort_click(int list, HWND lv, int col);

// ---- preview.cpp -- turning an ExtractedEntry into the visible preview surface
void update_preview_texture();
void populate_struct_tree();
// Runs populate_struct_tree() only if the Structure tab is the one currently
// showing and its content is stale (g_app->struct_tree_dirty); a no-op
// otherwise, cheap enough to call from relayout()/TCN_SELCHANGE on every tab
// click without re-parsing anything for tabs the user isn't looking at.
void populate_struct_tree_if_visible();
void apply_extracted_entry(uint32_t mft_index, ExtractedEntry&& entry);
void on_entry_selected(uint32_t mft_index);

// ---- file_ops.cpp -- open, export, load template/keys, search and filters
bool load_dat_path(HWND hwnd, const wchar_t* path);
void do_open_file(HWND hwnd);
void do_load_template(HWND hwnd);
void load_keys_from(const std::wstring& csv_path);
void do_load_keys(HWND hwnd);
void try_autoload_keys();
void do_export(HWND hwnd, bool export_compressed);
void do_export_gltf_model(HWND hwnd);
void do_export_gltf_model_atlas(HWND hwnd);
void do_export_gltf_map(HWND hwnd);
void on_gltf_export_done(HWND hwnd);
void do_export_vrchat_model(HWND hwnd);
void on_vrchat_model_done(HWND hwnd, LPARAM lparam);
void do_export_skybox_map(HWND hwnd);
void on_skybox_export_done(HWND hwnd, LPARAM lparam);
void do_save_model_texture(HWND hwnd, uint32_t fileId);
std::string combo_sel(HWND combo);
void apply_filters();
void do_open_loose_file(HWND hwnd);
void do_build_index(HWND hwnd);
bool load_index_path(HWND hwnd, const wchar_t* path);
void on_index_build_done(HWND hwnd, bool ok);
void do_search();
void do_clear_search();
void populate_struct_container_combo();  // preview.cpp -- lists this entry's own chunks (id + name), from the parsed tree
void on_struct_container_changed();       // preview.cpp -- combo selection -> jump the tree to that chunk

// ---- chat_link_dialog.cpp -- the &[base64] chat-link decoder popup
void cl_do_decode();
std::wstring cmap_cache_path();
uint32_t cl_content_id();
void cl_show_resolved();
void cl_open_fid(uint32_t fid);
void cl_resolve_asset();
void cl_search(bool by_file_id);
LRESULT CALLBACK ChatLinkWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
void open_chat_link_decoder(HWND owner);

// ---- content_map_service.cpp -- the shared cntc content-map build
enum class CmapEnsure { Ready, Building, Started, NeedDat, NeedIndex, InUse };
/// ensure_content_map's first answer: Building wins over built() (a build's
/// finalize already reports built), then Ready; nullopt = go on to load/build.
std::optional<CmapEnsure> ensure_precheck(bool building, bool built);
/// Why a rebuild may not discard the current map (nullopt = it may): a build is
/// running, a reader (character fetch) is using it, or there is no dat / index
/// to build a new one from.
std::optional<CmapEnsure> rebuild_precheck(bool building, int readers, bool dat_loaded, bool index_loaded);
/// Background readers of the shared cmap (character fetches) hold this for as
/// long as they read it; rebuild_content_map refuses (InUse) meanwhile.
void acquire_content_map_reader();
void release_content_map_reader();
int content_map_readers();
std::wstring cmap_cache_path();
/// Ready if built or the disk cache loads; Building/Started post WM_APP_CMAP_DONE
/// to `notify` when the background build finishes; NeedDat/NeedIndex: the caller
/// tells the user what to open first.
CmapEnsure ensure_content_map(HWND notify);
/// Drops the map and its disk cache, then ensure_content_map(notify) -- unless
/// rebuild_precheck() refuses, in which case nothing is touched.
CmapEnsure rebuild_content_map(HWND notify);
bool content_map_building();

// ---- content_names.cpp -- game names + chat links for what uses a file
/// The cached game name of a content object: nullptr = not fetched yet, "" = none.
const std::string* cached_content_name(uint32_t type, uint32_t id);
/// Queue names not cached yet for a background fetch from the public GW2 API;
/// `notify` gets WM_APP_CONTENT_NAMES_DONE as they arrive.
void request_content_names(HWND notify, const std::vector<castlemist::cmap::ContentRef>& refs);
/// The info panel's "Game content" section for entry `mft_index` (or the
/// content object selected in the cntc browser), queueing any missing names.
std::wstring content_links_text(uint32_t mft_index);
/// content_browser.cpp: an entry's game name for the entries table's Name column,
/// and filling those cells in once names arrive.
std::wstring content_object_name(const ContentObject& o);
void refresh_content_names();
/// The main list's Name column (mft::NameProvider): the first game name among
/// the objects that use any of `file_ids`, "+N" when more use it.
bool name_for_files(const std::vector<uint32_t>& file_ids, bool fetch, std::wstring& out);
/// Download the name of every API-nameable content object in the background
/// (building the content map first if needed), so browsing never waits on the
/// API and the search box can find anything by name. `quiet`: report problems
/// in the status bar instead of a message box.
void download_all_names(HWND hwnd, bool quiet);
void on_main_cmap_done(HWND hwnd);
void on_names_progress(size_t done, size_t total);
void on_names_bulk_done(bool ok, size_t total);
/// Base ids of every entry used by a content object whose cached name contains
/// `key` (name_query_key form). `named_total`: how many names are cached.
std::vector<uint32_t> base_ids_matching_name(const std::wstring& key, size_t* named_total);
/// Re-show the current entry's info panel (e.g. once names or the map arrive).
void refresh_entry_info();

/// Point the main browser at a dat fileId (file-id search) and bring it forward.
void navigate_to_file_id(uint32_t fid);

// ---- character_dialog.cpp / character_keys_dialog.cpp -- Character Ripper
void open_character_dialog(HWND owner);
void open_character_keys_dialog(HWND owner, std::function<void()> on_changed);
// look_dialog.cpp -- Character Ripper > Edit look...
void open_look_dialog(HWND owner, const castlemist::character::CharacterManifest& manifest);

// ---- dye_dialog.cpp -- the viewer's armor dye channels
/// Opens (or raises) the Dyes window for the current armor model.
void open_dye_dialog(HWND owner);
/// Tells an open Dyes window the model changed (channels, enabled state).
void dye_dialog_model_changed();
/// Re-bakes the current armor model's atlas with g_app->armor_dyes, or sets a
/// shader-dyed model's (a mount's) dye uniforms from g_app->shader_dyes, and
/// shows it in every view without reloading the model. False when the model
/// has no dye channels.
bool rebake_armor_dyes();
/// The dye picks the current model uses: shader_dyes or armor_dyes.
std::array<castlemist::ripper::DyeChoice, 4>& current_dyes();
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& w);

// ---- token_decoder_dialog.cpp -- token / filename-bytes decoder popup
LRESULT CALLBACK TokenDecoderWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
void open_token_decoder(HWND owner);

// ---- uv_map_dialog.cpp -- the UV map viewer popup
LRESULT CALLBACK UvMapWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
void open_uv_map_viewer(HWND owner);
void uv_map_notify_changed(); // no-op unless the popup is open

// ---- bake_select_dialog.cpp -- the atlas-bake material selection popup
struct BakeSelectResult {
    bool proceed = false;           ///< false = Cancel / closed -- abort the export entirely.
    std::set<uint32_t> selected;    ///< ModelMaterialCPU::index values to bake; the rest export plain.
};
BakeSelectResult show_bake_select_dialog(HWND owner, const ModelPreview& model);

// ---- index_ui.cpp -- opening a gw2index SQLite and wiring its filters
void finish_open_index(HWND hwnd, bool silent);
void do_open_index(HWND hwnd);
void try_autoload_index(HWND hwnd);

// ---- window_proc.cpp -- the menu and the three window procedures
HMENU build_menu();
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK ModelWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK BgfxWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
/// Hands the current entry to the "Game 1:1" surface, initialising bgfx on
/// first use. No-op unless the view is active and the entry is a model.
void sync_bgfx_view();
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

} // namespace castlemist::ui
