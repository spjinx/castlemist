/// @file
/// @brief Self-checks for the ported client logic.
///
/// These assert against values taken from the binary and from real archive
/// content, not against the ports' own output. Run it before trusting a change
/// to gr_fvf.h / gr_token.h / amat_effect.cpp / bgfx_draw.cpp.
///
/// Build standalone:
/// @code
/// g++ -std=c++20 -Iexternal/bgfx/include -Iexternal/bx/include \
///     -Iexternal/bx/include/compat/mingw \
///     tools/viewer/gw2bgfx/selftest.cpp tools/viewer/gw2bgfx/amat_effect.cpp \
///     tools/viewer/gw2bgfx/bgfx_draw.cpp external/bgfx/src/vertexlayout.cpp \
///     -Lbuild/relwithdebinfo/lib -lbgfx -lbimg -lbx -o build/gw2bgfx_selftest.exe
/// @endcode

#include <cstdio>
#include <cstdlib>

#include "amat_effect.h"
#include "amat_load.h"
#include "bgfx_draw.h"
#include "gr_fvf.h"
#include "gr_token.h"

using namespace gw2bgfx;

static int g_fail = 0;

#define CHECK(expr, ...)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("FAIL %s:%d  " #expr "\n      ", __FILE__, __LINE__);   \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Token -- base 23, alphabet "abcdefghiklmnopvrstuwxy"
// ---------------------------------------------------------------------------
static void testToken() {
    // Engine-global shader params, decoded in the client and recorded in
    // docs/research/gw2-uniform-hash.md.
    CHECK(tokenDecode32(0x8D43A284) == "grblcol", "got '%s'", tokenDecode32(0x8D43A284).c_str());
    CHECK(tokenDecode32(0xC04DF0F8) == "grbldir", "got '%s'", tokenDecode32(0xC04DF0F8).c_str());
    CHECK(tokenDecode32(0x3482C79B) == "grblsm",  "got '%s'", tokenDecode32(0x3482C79B).c_str());
    CHECK(tokenDecode32(0x3D55A0CC) == "grblsmb", "got '%s'", tokenDecode32(0x3D55A0CC).c_str());
    CHECK(tokenDecode32(0x368C09EC) == "grsmbs",  "got '%s'", tokenDecode32(0x368C09EC).c_str());

    // A technique quality token: 805394902 -> "high".
    CHECK(tokenDecode32(805394902u) == "high", "got '%s'", tokenDecode32(805394902u).c_str());
    CHECK(amatQualityOf(805394902u) == kQualityHigh, "quality rank wrong");

    // MODL material constants from model 143917.
    CHECK(tokenDecode32(0xBEF8FD7F) == "gloover", "got '%s'", tokenDecode32(0xBEF8FD7F).c_str());
    CHECK(tokenDecode32(0x3F45CA1E) == "gloptrb", "got '%s'", tokenDecode32(0x3F45CA1E).c_str());
    CHECK(tokenDecode32(0x54731015) == "glofade", "got '%s'", tokenDecode32(0x54731015).c_str());
    CHECK(tokenDecode32(0x894BBFD5) == "addnscl", "got '%s'", tokenDecode32(0x894BBFD5).c_str());

    // The encoder is the exact inverse, which is what makes name -> id lookups
    // safe to do at runtime instead of from a hand-kept table.
    const uint32_t kRoundTrip[] = {0x8D43A284, 0xC04DF0F8, 805394902u, 0xBEF8FD7F, 0x894BBFD5};
    for (uint32_t t : kRoundTrip)
        CHECK(tokenEncode32(tokenDecode32(t)) == t, "round trip failed for 0x%08X", t);
}

// ---------------------------------------------------------------------------
// GrFvf
// ---------------------------------------------------------------------------
static void testFvf() {
    // The client asserts DDI_STRIDE(fvf) == layout.getStride() at
    // BgfxBuffer.cpp:755. Check that on the fvfs real content actually uses --
    // 0x30081 and 0x70081 are jade-tech model 291977's (packed tangent frame
    // plus 2 and 3 half-precision UV channels).
    const uint32_t kRealFvfs[] = {
        0x00000101, // position + 1 float UV                        (simplest)
        0x00030081, // position + tangent frame + 2 half UVs        (291977)
        0x00070081, // position + tangent frame + 3 half UVs        (291977)
        0x00000187, // position + weights + group + tangent frame + 1 float UV
        0x0003008F, // the above skinned, with half UVs
    };
    for (uint32_t fvf : kRealFvfs) {
        bgfx::VertexLayout layout = grFvfBuildVertexLayout(fvf);
        CHECK(grFvfStrideMatches(fvf, layout),
              "fvf 0x%08X: DDI stride %u != layout stride %u",
              fvf, grFvfDdiStride(fvf), layout.getStride());
    }

    // Spot-check the two strides independently of each other.
    CHECK(grFvfDdiStride(0x00030081) == 32, "got %u", grFvfDdiStride(0x00030081));
    CHECK(grFvfDdiStride(0x00070081) == 36, "got %u", grFvfDdiStride(0x00070081));

    // Texcoord counts are highestSetBit-based, not popcount: the client relies
    // on the channels being contiguous from bit 8 / bit 16 upward.
    CHECK(grFvfTexCoordCountF32(0x00000000) == 0, "empty must be 0");
    CHECK(grFvfTexCoordCountF32(0x00000100) == 1, "got %u", grFvfTexCoordCountF32(0x00000100));
    CHECK(grFvfTexCoordCountF32(0x0000FF00) == 8, "got %u", grFvfTexCoordCountF32(0x0000FF00));
    CHECK(grFvfTexCoordCountF16(0x00030000) == 2, "got %u", grFvfTexCoordCountF16(0x00030000));
    CHECK(grFvfTexCoordCountF16(0x00FF0000) == 8, "got %u", grFvfTexCoordCountF16(0x00FF0000));

    // Element order is the byte order. Position must be first, and a packed
    // tangent frame must expand to three separate 4-byte attributes.
    bgfx::VertexLayout l = grFvfBuildVertexLayout(0x00030081);
    CHECK(l.getOffset(bgfx::Attrib::Position)  == 0,  "position not at 0");
    CHECK(l.getOffset(bgfx::Attrib::Normal)    == 12, "normal at %u", l.getOffset(bgfx::Attrib::Normal));
    CHECK(l.getOffset(bgfx::Attrib::Tangent)   == 16, "tangent at %u", l.getOffset(bgfx::Attrib::Tangent));
    CHECK(l.getOffset(bgfx::Attrib::Bitangent) == 20, "bitangent at %u", l.getOffset(bgfx::Attrib::Bitangent));
    CHECK(l.getOffset(bgfx::Attrib::TexCoord0) == 24, "uv0 at %u", l.getOffset(bgfx::Attrib::TexCoord0));
    CHECK(l.getOffset(bgfx::Attrib::TexCoord1) == 28, "uv1 at %u", l.getOffset(bgfx::Attrib::TexCoord1));
    CHECK(l.has(bgfx::Attrib::Color0) == false, "colour must be absent");
}

// ---------------------------------------------------------------------------
// Vertex shader variant, from the mesh draw loop
// ---------------------------------------------------------------------------
static void testVsVariant() {
    CHECK(vsVariantFromMeshFlags(0,     0, false) == kVsPlain,           "plain");
    CHECK(vsVariantFromMeshFlags(0,     0, true)  == kVsPlainInstanced,  "plain+instanced");
    CHECK(vsVariantFromMeshFlags(0x80,  0, false) == kVsFlag80,          "0x80");
    CHECK(vsVariantFromMeshFlags(0x80,  0, true)  == kVsFlag80Instanced, "0x80+instanced");
    // Weights|indices (0x2|0x4) wins over 0x80, because the client tests it
    // first -- and this is the branch that reaches the shader reading `grbones`.
    CHECK(vsVariantFromMeshFlags(0x86,  0, false) == kVsSkinned,         "skinned");
    // A real skinned geoset's FVF must land on the skinned feed, not on 0x80's:
    // fvf 0x00010087 is POSITION|WEIGHTS|GROUP|TANGENT_FRAME|texcoord, and every
    // rigged model in the archive looks like this.
    CHECK(vsVariantFromMeshFlags(0x00010087u, 0, false) == kVsSkinned,   "real skinned fvf");
    // ...unless the surface's own 0x2 vetoes it, which drops it to 0x80's feed.
    CHECK(vsVariantFromMeshFlags(0x86,  2, false) == kVsFlag80,          "veto -> 0x80");
}

// ---------------------------------------------------------------------------
// Draw state
// ---------------------------------------------------------------------------
static void testDrawState() {
    GrSurfaceState surf;

    // Write mask: plain colour effect writes RGBA.
    CHECK(grWriteMask(0x1, 0) == (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A), "plain mask");
    // 0x4000 = no alpha write -> RGB only. This is the flag that made a real
    // frame's blended draws all use mask 7, not the blending itself.
    CHECK(grWriteMask(0x4001, 0) == BGFX_STATE_WRITE_RGB, "no-alpha mask");
    // 0x4 = depth-only pass -> nothing at all.
    CHECK(grWriteMask(0x4, 0) == 0, "depth-only mask");
    // 0x8 = no RGB, alpha only.
    CHECK(grWriteMask(0x8, 0) == BGFX_STATE_WRITE_A, "alpha-only mask");
    // The surface's own flags reach the same result independently.
    CHECK(grWriteMask(0, 0x400) == BGFX_STATE_WRITE_RGB, "surface no-alpha");
    CHECK(grWriteMask(0, 0x800) == BGFX_STATE_WRITE_A,   "surface no-rgb");

    // Cull: shaderPassFlags bit 0 = CCW, bit 1 = CW. (Older notes read bit 0
    // as "opaque"; it is the cull mode, set in the pass builder.)
    CHECK(grCullState(0x1, 0) == BGFX_STATE_CULL_CCW, "cull ccw");
    CHECK(grCullState(0x2, 0) == BGFX_STATE_CULL_CW,  "cull cw");
    CHECK(grCullState(0x1, 0, /*mirrored=*/true) == BGFX_STATE_CULL_CW, "mirrored swap");
    // A shader-level flag overrides the effect's.
    CHECK(grCullState(0x1, kShaderCullCw) == BGFX_STATE_CULL_CW,   "shader cw wins");
    CHECK(grCullState(0x1, kShaderCullNone) == 0,                  "shader none wins");

    // Depth: pass 0 lays depth down with LEQUAL, later passes match with EQUAL.
    GrDrawState p0 = grComputeDepthState(0, 0x1, surf);
    CHECK((p0.state & BGFX_STATE_DEPTH_TEST_MASK) == BGFX_STATE_DEPTH_TEST_LEQUAL, "pass0 leq");
    CHECK((p0.state & BGFX_STATE_WRITE_Z) != 0, "pass0 writes z");
    GrDrawState p1 = grComputeDepthState(1, 0x1, surf);
    CHECK((p1.state & BGFX_STATE_DEPTH_TEST_MASK) == BGFX_STATE_DEPTH_TEST_EQUAL, "pass1 eq");

    // 0x40 = translucent: still tests, stops writing.
    GrDrawState tr = grComputeDepthState(0, 0x41, surf);
    CHECK((tr.state & BGFX_STATE_WRITE_Z) == 0, "translucent must not write z");
    // 0x20 = depth test off entirely.
    GrDrawState nz = grComputeDepthState(0, 0x21, surf);
    CHECK((nz.state & BGFX_STATE_DEPTH_TEST_MASK) == 0, "depth test must be off");

    // The decal-family material tokens pull toward the camera.
    surf.materialToken = 0x48A635447ull;
    CHECK(grComputeDepthState(0, 0x1, surf).depthBias == 65.0f, "decal bias");
    surf.materialToken = 0;
    CHECK(grComputeDepthState(0, 0x8001, surf).depthBias == -65.0f, "push bias");

    // Full composition: the effect's renderState must survive into the state
    // word, and the write mask must NOT be left at zero.
    AmatEffect eff;
    eff.shaderPassFlags = 0x4001;
    eff.renderState = 0x0000000006565000ull;  // the alpha-blend word seen in real AMATs
    GrSurfaceState s2;
    GrDrawState composed = grComposeDrawState(eff, 0, s2);
    CHECK((composed.state & eff.renderState) == eff.renderState, "renderState lost");
    CHECK((composed.state & BGFX_STATE_WRITE_RGB) != 0, "colour write mask was zeroed");
    CHECK((composed.state & BGFX_STATE_CULL_CCW) != 0, "cull from passFlags bit 0");

    // Two-sided suppresses the cull bits.
    s2.materialFlags = 0x4000;
    CHECK((grComposeDrawState(eff, 0, s2).state & BGFX_STATE_CULL_MASK) == 0, "two-sided");
}

// ---------------------------------------------------------------------------
// Effect selection
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// DXBC resource declarations -- which texture registers are cubes. GW2's bgfx
// blobs leave the per-sampler texInfo at 0, so the shader code is the only
// place the dimension lives (Twilight's cubemap material crashed the debug
// layer on a 2D view bound to a TextureCube register).
// ---------------------------------------------------------------------------
static void testDxbcResourceDimensions() {
    auto put = [](std::vector<uint8_t>& b, uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    std::vector<uint32_t> shex = {
        0x00000050u,                                   // ps_5_0
        0u,                                            // length, patched below
        0x58u | (3u << 11) | (4u << 24), 0x00107000u, 0u, 0x5555u,   // dcl_resource_texture2d t0
        0x58u | (6u << 11) | (4u << 24), 0x00107000u, 1u, 0x5555u,   // dcl_resource_texturecube t1
        0x80000000u | 0x45u | (5u << 24), 0x00000001u, 0x00100000u, 0u, 0u,  // sample with an extended token
        0x3Eu | (1u << 24),                            // ret
    };
    shex[1] = (uint32_t)shex.size();
    std::vector<uint8_t> dxbc = {'D', 'X', 'B', 'C'};
    for (int i = 0; i < 16; ++i) dxbc.push_back(0);   // checksum
    put(dxbc, 1);
    const uint32_t chunkSize = (uint32_t)shex.size() * 4;
    put(dxbc, 36 + 8 + chunkSize);                     // total size
    put(dxbc, 1);                                      // chunk count
    put(dxbc, 36);                                     // chunk offset
    dxbc.insert(dxbc.end(), {'S', 'H', 'E', 'X'});
    put(dxbc, chunkSize);
    for (uint32_t t : shex) put(dxbc, t);

    std::vector<uint8_t> blob = {'F', 'S', 'H', 11};   // DXBC embedded in a bgfx blob
    blob.insert(blob.end(), dxbc.begin(), dxbc.end());
    const auto dims = dxbcResourceDimensions(blob);
    CHECK(dims.size() == 2, "got %zu declarations", dims.size());
    CHECK(dims.count(0) && dims.at(0) == kDxbcTexture2D, "t0 should be 2D");
    CHECK(dims.count(1) && dims.at(1) == kDxbcTextureCube, "t1 should be a cube");
    CHECK(dxbcResourceDimensions({'F', 'S', 'H', 11}).empty(), "no DXBC -> no declarations");
}

static void testEffectSelection() {
    AmatPackage pkg;
    pkg.shaders.resize(8);

    AmatTechnique tech;
    tech.quality = 805394902u;   // "high"
    tech.passes.resize(2);

    auto makeEffect = [](uint64_t token, uint32_t ps) {
        AmatEffect e;
        e.token = token;
        e.pixelShaderIndex = ps;
        e.vertexShaderVariants.push_back({kVsPlain, 1});
        e.vertexShaderVariants.push_back({kVsSkinned, 2});
        e.vertexShaderVariants.push_back({kVsFlag80, 3});
        return e;
    };

    const uint64_t kMatToken = 0x1234500001ull;
    tech.passes[0].effects.push_back(makeEffect(AmatTokenChain::kDefault, 3));
    tech.passes[0].effects.push_back(makeEffect(kMatToken, 4));
    tech.passes[1].effects.push_back(makeEffect(AmatTokenChain::kDefault, 5));
    pkg.techniques.push_back(tech);

    CHECK(amatSelectTechnique(pkg, kQualityHigh) == 0, "technique pick");

    // The material's own token wins over the default, even though the default
    // appears first in the effect list.
    AmatSelection s = amatSelectEffect(pkg, 0, 0, kMatToken, kVsPlain);
    CHECK(s.ok && s.pixelShaderIndex == 4, "material token should win");
    CHECK(s.matchedMaterialToken, "should report a real token match");

    // An unknown token falls back to the default -- on pass 0 only.
    AmatSelection d0 = amatSelectEffect(pkg, 0, 0, 0xDEADBEEFull, kVsPlain);
    CHECK(d0.ok && d0.pixelShaderIndex == 3, "pass 0 default fallback");
    CHECK(!d0.matchedMaterialToken, "default is not a token match");

    // On a later pass the client draws nothing instead. Substituting the
    // default here would render geometry the game never draws.
    AmatSelection d1 = amatSelectEffect(pkg, 0, 1, 0xDEADBEEFull, kVsPlain);
    CHECK(!d1.ok, "pass 1 must NOT fall back to the default");

    // The remap chain: kRemapFrom0 must find kRemapTo0.
    tech.passes[0].effects.push_back(makeEffect(AmatTokenChain::kRemapTo0, 6));
    pkg.techniques[0] = tech;
    AmatSelection r = amatSelectEffect(pkg, 0, 0, AmatTokenChain::kRemapFrom0, kVsPlain);
    CHECK(r.ok && r.pixelShaderIndex == 6, "remap chain");

    // The client's retry is on the raw ids: 4 degrades to 2, everything else to
    // 0 (BgfxShader_FindVsVariant, `v7 = 2; if (a1 != 4) v7 = 0;`).
    AmatSelection v = amatSelectEffect(pkg, 0, 0, kMatToken, kVsFlag80Instanced);
    CHECK(v.ok && v.variant == kVsFlag80, "4 must degrade to 2, got %u", v.variant);

    // And an absent SKINNED request degrades all the way to the plain feed, not
    // to 2 -- so an effect with no variant 1 simply draws unskinned, which is
    // what the client does too.
    AmatEffect noSkin;
    noSkin.token = kMatToken;
    noSkin.pixelShaderIndex = 7;
    noSkin.vertexShaderVariants.push_back({kVsPlain, 1});
    noSkin.vertexShaderVariants.push_back({kVsFlag80, 3});
    AmatTechnique t2 = tech;
    t2.passes[0].effects.clear();
    t2.passes[0].effects.push_back(noSkin);
    pkg.techniques[0] = t2;
    AmatSelection ns = amatSelectEffect(pkg, 0, 0, kMatToken, kVsSkinned);
    CHECK(ns.ok && ns.variant == kVsPlain, "absent skinned -> plain, got %u", ns.variant);
}

// Every pass the client draws for one material, in pass order. A material can
// paint nothing in pass 0 and everything in a later pass: AMAT 543769 (the
// Holographic Dawn blade) has only depth/StencilId effects in pass 0 and its
// additive glow in pass 1, both answering the opaque render-mode token. Drawing
// pass 0 alone rendered the blade invisible. A pass that does not carry the
// token is still skipped -- the default is pass-0-only.
static void testPassEnumeration() {
    AmatPackage pkg;
    pkg.shaders.resize(8);
    AmatTechnique tech;
    tech.quality = 805394902u;
    tech.passes.resize(3);
    auto makeEffect = [](uint64_t token, uint32_t ps) {
        AmatEffect e;
        e.token = token;
        e.pixelShaderIndex = ps;
        e.vertexShaderVariants.push_back({kVsPlain, 1});
        return e;
    };
    const uint64_t kOpaque = 0x914C6A8A883B1EEull;
    tech.passes[0].effects.push_back(makeEffect(kOpaque, 3));
    tech.passes[1].effects.push_back(makeEffect(kOpaque, 4));
    tech.passes[2].effects.push_back(makeEffect(AmatTokenChain::kDefault, 5));
    pkg.techniques.push_back(tech);

    const auto passes = amatSelectPasses(pkg, 0, kOpaque, kVsPlain);
    CHECK(passes.size() == 2, "expected passes 0 and 1, got %zu", passes.size());
    if (passes.size() == 2) {
        CHECK(passes[0].first == 0 && passes[0].second.pixelShaderIndex == 3, "pass 0 first");
        CHECK(passes[1].first == 1 && passes[1].second.pixelShaderIndex == 4, "pass 1 second");
    }
    CHECK(amatSelectPasses(pkg, 5, kOpaque, kVsPlain).empty(), "bad technique -> nothing");
}

int main() {
    testToken();
    testFvf();
    testVsVariant();
    testDrawState();
    testEffectSelection();
    testPassEnumeration();
    testDxbcResourceDimensions();

    if (g_fail == 0) std::printf("gw2bgfx selftest: all checks passed\n");
    else             std::printf("gw2bgfx selftest: %d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
