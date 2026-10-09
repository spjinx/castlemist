#ifndef CASTLEMIST_RIPPER_SHADER_DYE_H
#define CASTLEMIST_RIPPER_SHADER_DYE_H

// Dyes the game applies in the pixel shader rather than baking into a texture:
// mounts (and any other model whose materials declare the `hsmnt*` uniforms).
// The material carries a `dyemask` texture, whose R/G/B/A are the weights of dye
// channels 1-4, and twelve float4 uniforms, three affine rows per channel:
//
//   channel 1: hsmnta hsmntb hsmntc     channel 3: hsmntg hsmnth hsmnti
//   channel 2: hsmntd hsmnte hsmntf     channel 4: hsmntl hsmntm hsmntn
//
// and the shader does `c = lerp(c, dot(row_r|g|b, float4(albedo.rgb, 1)), mask_i)`
// per channel (AMAT 1749831 ps 68, the springer). The MODL ships them as identity
// rows, so a model previews as authored until a dye is picked. See
// docs/research/gw2-shader-dyes.md.

#include <array>
#include <map>
#include <optional>
#include <string>

#include "castlemist/extract/model_types.h"
#include "castlemist/ripper/dye.h"

namespace castlemist::ripper {

/// The dye uniforms' names, [channel][row]: row 0/1/2 makes red/green/blue.
inline constexpr const char* kDyeUniforms[4][3] = {{"hsmnta", "hsmntb", "hsmntc"},
                                                    {"hsmntd", "hsmnte", "hsmntf"},
                                                    {"hsmntg", "hsmnth", "hsmnti"},
                                                    {"hsmntl", "hsmntm", "hsmntn"}};

using DyeRows = std::array<std::array<float, 4>, 3>;
using ShaderUniforms = std::map<std::string, std::array<float, 4>>;

/// The shader's three rows for @p m: dye_matrix() works on 0..255 BGR, the shader
/// on 0..1 RGB, so the rows and columns are reversed and the offset scaled by
/// 1/255. `dot(row, (rgb/255, 1)) * 255` equals apply_dye()'s result unclamped.
DyeRows dye_shader_rows(const ColorMatrix& m);

/// True when any material of @p model declares a dye uniform: its colour is
/// dyed in the shader, and set_shader_dyes() can change it.
bool has_shader_dyes(const ModelPreview& model);

/// Which channels have any weight in some material's dyemask (a channel the
/// mask never covers does nothing, whatever it is dyed).
std::array<bool, 4> shader_dye_channels(const ModelPreview& model);

/// The uniform values for one dye per channel; nullopt leaves that channel as
/// authored (identity rows).
ShaderUniforms shader_dye_uniforms(const std::array<std::optional<ColorMatrix>, 4>& dyes);

/// Writes @p uniforms into the model as the game would draw it: the game-shader
/// constants (Shader mode) of every material that declares them, and, for the
/// reconstruction (Full mode), a copy of each dyed material's diffuse with the
/// dyes baked through its dyemask. Starts from @p pristine, the model as
/// extracted, so it can be called again with other dyes; @p model must be a
/// copy of it. Returns the number of materials changed.
int set_shader_dyes(ModelPreview& model, const ModelPreview& pristine, const ShaderUniforms& uniforms);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_SHADER_DYE_H
