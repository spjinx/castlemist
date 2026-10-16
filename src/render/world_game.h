/// @file
/// @brief WorldView's Shader shading: the game's own bgfx DXBC material
///        shaders (ModelPreview::gameMaterials) on WorldView's device, for
///        the props near the camera.
///
/// Mirrors the main window's map-scene Shader path (scene.cpp
/// render_scene_game, game_material.cpp build_one_game_mat): one draw per
/// instance and submesh, the instance's World / WorldViewProjection /
/// WorldView written into the material's vertex cbuffer per draw, GVertex
/// vertices, the env rig's uniforms. What differs is only what WorldView
/// needs: its own device, textures from its TextureCache (capped at
/// WorldMaterialSettings::maxTextureSize), shaders and layouts shared across
/// materials by their DXBC bytes, one dynamic cbuffer per size instead of
/// two per material, and the build split out so it can run under a time
/// budget. The light buffer (sampler role 34, and the other engine globals
/// the extraction marks 1) comes from the light pre-pass the main window
/// runs (lighting.cpp: a world-normal G-buffer of the drawn opaque
/// submeshes, then kLightAccum off the env rig's SH uniforms), over the
/// game-drawn instances only.
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "castlemist/extract/model_types.h"
#include "castlemist/render/world_prep.h"

#include "detail/game_uniforms.h"
#include "detail/math.h"
#include "world_materials.h"

namespace castlemist::render {

/// The engine uniforms that change per frame or per instance.
enum class GameDyn : uint8_t { World, WorldViewProjection, WorldView, ViewProjection, View, CameraPosition, ScreenDims,
                               Time, Count };

/// The GameDyn a bgfx uniform name is, or Count for any other.
GameDyn game_dyn_of(const std::string& name);

/// One material as the pass draws it.
struct GameMatWorld {
    ID3D11VertexShader* vs = nullptr;   ///< owned by the pass's program cache
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* il = nullptr;
    ID3D11BlendState* blend = nullptr;        ///< non-null: drawn in the blended pass
    ID3D11BlendState* opaqueBlend = nullptr;  ///< blending off, the effect's write mask
    bool depthWrite = true;                   ///< shaderPassFlags & 0x40 clears it
    /// Per PS slot: a material texture by fileId (TextureCache, UNORM view),
    /// or one of the stand-ins (GameSamplerCPU::global: 1 grey light buffer,
    /// 2 grey env cube, 3 white shadow map, 4 far depth). -1 = unbound.
    uint32_t texFile[16] = {};
    int8_t global[16];
    /// The cbuffers: their bytes with every static uniform and material
    /// constant in place, and where the GameDyn values go.
    struct Dyn { uint16_t off; GameDyn var; uint8_t vec4s; };
    std::vector<uint8_t> vsBase, psBase;
    std::vector<Dyn> vsDyn, psDyn;
    bool lightStandIn = false;   ///< some slot is a global 1 (light buffer / other)
    /// The light pre-pass alpha-tests slot 0 (GameMaterial::prepassCutout).
    bool cutout = false;
    bool ok = false;
    GameMatWorld() { for (auto& g : global) g = -1; }
};

/// One model's game-shader state.
struct GameModelWorld {
    enum class State : uint8_t { NoData, Waiting, Ready, NoShader };
    std::shared_ptr<const ModelPreview> game;   ///< add(): gameMaterials + texture headers
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb, ib;
    struct Sub { uint32_t first = 0, count = 0, mat = 0; };
    std::vector<Sub> subs;            ///< LOD0, one per non-empty mesh
    std::vector<GameMatWorld> mats;   ///< by ModelMaterialCPU::index
    State state = State::NoData;
    bool built = false;               ///< build() ran (Ready or NoShader)
    bool texturesSettled = false;     ///< every material texture Ready or Failed
    bool lightStandIn = false;
};

/// Per-frame values of the GameDyn uniforms (16 floats each; the bgfx
/// matrices are the transposed row-vector Mat4s, as scene.cpp writes them).
struct GameFrame {
    float v[static_cast<int>(GameDyn::Count)][16] = {};
    Mat4 view{}, viewProj{};
};

class WorldGamePass {
public:
    /// The stand-in textures and the sampler. False on failure.
    bool create(ID3D11Device* dev);
    /// Drops every model's state (set_world); keeps the program cache.
    void reset(size_t models);
    /// Records a traced model; false when @p m is out of range or already had data.
    bool add(uint32_t m, std::shared_ptr<const ModelPreview> game);
    /// @p m's state, with @p preview the model's geometry (null: not in yet):
    /// NoData before add(), Waiting until build() ran and every material
    /// texture is settled in @p tex, then Ready or NoShader.
    GameModelWorld::State state(uint32_t m, const ModelPreview* preview, const TextureCache& tex);
    /// Whether @p m has data and geometry but has not been built yet.
    bool needs_build(uint32_t m, const ModelPreview* preview) const;
    /// Creates @p m's GPU state from its game data and @p preview's LOD0
    /// meshes. NoShader when any submesh's material has no usable game
    /// shader (the whole model then falls back).
    void build(ID3D11Device* dev, uint32_t m, const ModelPreview& preview, const GameVals& vals);
    /// The frame's camera values (call once per frame before draw_instance).
    void begin_frame(const GameFrame& f) { frame_ = f; }
    /// Binds the shared sampler on slots 0-15 and the topology.
    void bind_common(ID3D11DeviceContext* ctx);
    /// Draws instance @p world of Ready model @p m: the opaque submeshes
    /// (@p blendedPass false) or the blended ones. Returns draw calls, adds
    /// triangles to @p tris.
    int draw_instance(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t m, const Mat4& world, bool blendedPass,
                      const TextureCache& tex, ID3D11DepthStencilState* dss, ID3D11DepthStencilState* dssNoWrite,
                      size_t& tris);
    /// Compiles the light pre-pass (kNormalGBuffer, kLightAccum). False on failure.
    bool create_prepass(ID3D11Device* dev);
    /// (Re)creates the pre-pass targets at @p w x @p h; false on failure
    /// (the pass then binds the grey stand-in).
    bool resize_prepass(ID3D11Device* dev, int w, int h);
    /// The light pre-pass over the opaque submeshes of @p insts (model,
    /// world) using @p dsv (cleared here): fills the light buffer the draws
    /// then bind at their global-1 slots. @p vals carries the SH rig. Leaves
    /// the context's targets unbound.
    void light_prepass(ID3D11DeviceContext* ctx, const std::vector<std::pair<uint32_t, const Mat4*>>& insts,
                       ID3D11DepthStencilView* dsv, ID3D11DepthStencilState* dss, ID3D11RasterizerState* rs,
                       const TextureCache& tex, const GameVals& vals, int w, int h);
    /// Forgets the frame's light buffer (the next draws bind the stand-in).
    void clear_light() { lightValid_ = false; }
    /// Draws the opaque submeshes of Ready model @p m for every world in
    /// @p worlds: each material is bound once, then per instance only the
    /// cbuffers that read per-instance values are rewritten. Returns draw calls.
    int draw_opaque_group(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t m, const std::vector<const Mat4*>& worlds,
                          const TextureCache& tex, ID3D11DepthStencilState* dss, size_t& tris);
    /// Whether Ready model @p m has any blended submesh.
    bool has_blended(uint32_t m) const;

    int models_built() const { return built_; }
    int models_failed() const { return failed_; }
    int programs() const { return static_cast<int>(vs_.size() + ps_.size()); }
    int light_stand_ins() const { return lightStandIns_; }

private:
    bool build_mat(ID3D11Device* dev, const GameMaterial& c, const ModelPreview& game, const GameVals& vals,
                   GameMatWorld& g);
    ID3D11Buffer* cbuffer(ID3D11Device* dev, size_t bytes, bool vs);
    void write_cb(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const std::vector<uint8_t>& base,
                  const std::vector<GameMatWorld::Dyn>& dyn);
    /// Binds @p g's shaders, layout, cbuffers and textures (not blend/depth).
    void bind_material(ID3D11DeviceContext* ctx, const GameMatWorld& g, ID3D11Buffer* vcb, ID3D11Buffer* pcb,
                       const TextureCache& tex);
    /// inst_ for one instance.
    void set_instance(const Mat4& world);

    std::vector<GameModelWorld> models_;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11VertexShader>> vs_;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11PixelShader>> ps_;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11InputLayout>> il_;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11BlendState>> blend_;
    std::unordered_map<size_t, Microsoft::WRL::ComPtr<ID3D11Buffer>> vsCb_, psCb_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> grey_, envCube_, white_, farDepth_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    // Light pre-pass.
    Microsoft::WRL::ComPtr<ID3D11VertexShader> nrmVs_, lightVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> nrmPs_, lightPs_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> nrmIl_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> nrmCb_, lightCb_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> nrmRtv_, lightRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nrmSrv_, lightSrv_;
    bool lightValid_ = false;
    GameFrame frame_;
    float inst_[3][16] = {};   ///< World, WorldViewProjection, WorldView of the instance being drawn
    std::vector<uint8_t> scratch_;
    int built_ = 0, failed_ = 0, lightStandIns_ = 0;
};

} // namespace castlemist::render
