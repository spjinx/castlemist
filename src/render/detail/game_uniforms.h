/// @file
/// @brief The device-free half of the game-shader (bgfx DXBC) material path:
///        the engine uniform values, the cbuffer fill, the blend and
///        input-layout descriptions.
///
/// Shared by the main renderer's Shader mode (game_material.cpp, which owns
/// the device globals in state.h) and WorldView's Shader shading
/// (world_game.cpp, its own device). Nothing here touches a device.
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "castlemist/extract/map_types.h"
#include "castlemist/extract/model_types.h"

namespace castlemist::render {

/// bgfx uniform name -> value (float4 blocks, matrices as 16 floats).
using GameVals = std::map<std::string, std::vector<float>>;

/// @brief The engine uniforms that do not come from the light rig or the
///        camera: TexTransforms, LightBuffer, fxclr, AlphaRef, fog / shadow /
///        local-light switches (game_material.cpp documents each value).
///        @p lit is the GW2_LIGHT brightness knob (1 = neutral).
void game_base_uniforms(GameVals& vals, float lit);

/// @brief The map-scene light rig as the game's forward materials read it:
///        shSun / shSunData = @p sunToward normalised, shSunColor and the SH
///        ambient DC from calibrated_light(@p r), backlight off, SunColor 0,
///        SunDirection = -shSun (the travel direction).
void game_map_env_uniforms(GameVals& vals, const MapEnvRig& r, const float sunToward[3], float lit);

/// @brief The render-target write mask of an effect's shaderPassFlags.
UINT8 game_write_mask(uint32_t passFlags);
/// @brief The blend description of a bgfx state word's blend fields; false
///        (and @p out untouched) when the word has no blend bits.
bool game_blend_desc(uint64_t state, uint32_t passFlags, D3D11_BLEND_DESC& out);
/// @brief Blending off with the effect's own write mask.
D3D11_BLEND_DESC game_opaque_blend_desc(uint32_t passFlags);

/// @brief GVertex byte offset of a VS input semantic.
UINT game_sem_off(const char* semantic, UINT index);
/// @brief The input layout for GVertex (stride sizeof(GVertex)) read off the
///        VS input signature. @p names keeps the semantic strings the
///        descriptions point at alive. False when the DXBC cannot be reflected.
bool game_input_layout(const std::vector<uint8_t>& vsDXBC, std::vector<std::string>& names,
                       std::vector<D3D11_INPUT_ELEMENT_DESC>& out);

/// @brief Byte size of a material cbuffer: the largest live register, 16-byte aligned, at least 16.
UINT game_cb_size(const std::vector<GameShaderUniform>& u, uint32_t constBuf);
/// @brief Zeroes @p buf, writes every uniform found in @p vals at its offset,
///        then the material's own constants over them.
void game_fill_cb(std::vector<uint8_t>& buf, const std::vector<GameShaderUniform>& u, const GameVals& vals,
                  const std::vector<GameConstOverride>& consts);

} // namespace castlemist::render
