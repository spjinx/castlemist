/// @file
/// @brief The zone scatter's phase 2 (docs/superpowers/specs/2026-10-16-world-foliage-design.md
///        D5): the client's exact expansion of a zon2 cell into instances, transcribed from
///        Gw2-64.exe (PE timestamp 1792004482, the disable-ASLR copy, image base 0x140000000)
///        as docs/research/gw2-terrain-to-unity.md §1.3 "2026-10-16: zone scatter rule"
///        records it, and the labelled densityMatched fallback.
///
/// Every float operation below is single precision, one rounding per operation, in the
/// client's order: the exact rule quantises the scale and the jitter, so the order matters.

#include "castlemist/world/zone_scatter.h"

#include "castlemist/world/props.h"
#include "castlemist/world/terrain.h"
#include "castlemist/world/world_scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>
#include <unordered_map>

namespace castlemist::world {

namespace {

// Gw2-64.exe (PE timestamp 1792004482, disable-ASLR copy, image base 0x140000000), .data 0x142592280:
// the 256-entry per-cell seed table (count 0x100 at 0x142592680), read statically.
constexpr uint32_t kSeedTable[256] = {
    0x35A9260D, 0x15CE3E30, 0x223DF0C1, 0x2C97D363, 0x22B5231B, 0x02659C70,
    0x1F1CFC5E, 0x1C3D7526, 0x1E708AE8, 0x39B2110E, 0x17BD18F0, 0x041F9AB0,
    0x0579E352, 0x29C3F90F, 0x361C7701, 0x1762084A, 0x1711DA2E, 0x18D8E1D0,
    0x14DC638D, 0x05F94479, 0x0ECEE94E, 0x3A7F46D5, 0x39062864, 0x320EDEB4,
    0x046EBCFF, 0x12C20F79, 0x301E3C1D, 0x0B5EB8A5, 0x17EB43D1, 0x06C25884,
    0x32D568A1, 0x2B170D3F, 0x0AD1AB80, 0x13C35140, 0x38F8B432, 0x03D87273,
    0x2F5F35BF, 0x2733C29C, 0x17F4AF41, 0x088B5883, 0x3703DF84, 0x35494D73,
    0x28AB0231, 0x0B200D68, 0x0417F3D8, 0x0514566D, 0x273DD5B8, 0x25FAF20C,
    0x08A0CC5C, 0x00660990, 0x2D73A715, 0x31FDFC19, 0x28645FB1, 0x1FA0C886,
    0x367816D1, 0x24836935, 0x08F20CDD, 0x32674F7B, 0x2DD6B8E8, 0x3B2910C8,
    0x129EAD63, 0x0705CEA9, 0x01A8F2FD, 0x0598B9EB, 0x3489B1BE, 0x231530D1,
    0x15864712, 0x23B6B4AE, 0x3667B2DA, 0x26C02B90, 0x37E5EF8E, 0x27C1965C,
    0x32CBD644, 0x0CD2C897, 0x0B198B44, 0x17F38938, 0x0F4B711C, 0x1F6C2C91,
    0x0FD030B9, 0x16030550, 0x18BC3218, 0x2D99CBC9, 0x24DB0521, 0x12FAEC57,
    0x34EED239, 0x3B6EC6D1, 0x1F79C097, 0x11F69C20, 0x1518D195, 0x1F75D723,
    0x010D43E7, 0x3AA0FA48, 0x281C5D4A, 0x14FD7A37, 0x23D64B5F, 0x1EF049D3,
    0x0E85D570, 0x1312E72E, 0x38BA4C42, 0x378ACBF1, 0x1FAB52C7, 0x11862E97,
    0x3A4EBD8D, 0x0176EA1F, 0x0963088C, 0x2FF2EABD, 0x2042F33E, 0x13FF2D94,
    0x00D541B9, 0x0AE30983, 0x006BAD76, 0x06348FF2, 0x35BC4E8F, 0x33DF49A3,
    0x1AC43E96, 0x1D534A99, 0x153AD6F6, 0x06F8DB89, 0x24C86E46, 0x2FC12723,
    0x0005B67B, 0x37D72E3C, 0x19DD9577, 0x2AFAF18D, 0x203786EF, 0x0693AA1E,
    0x129BBEF0, 0x07748661, 0x1FCC882E, 0x044B3FED, 0x2E359AC6, 0x08FF844E,
    0x15FA61DF, 0x3B750F26, 0x1EDFF0A8, 0x249D2BA6, 0x3991BD79, 0x12B2DD76,
    0x16BED853, 0x316DC827, 0x2F162C50, 0x00989433, 0x2C150F37, 0x05C39C73,
    0x298857AF, 0x0A262198, 0x2E0F5F80, 0x3333CA7C, 0x11B7304A, 0x37B78538,
    0x096CC06D, 0x2C799AFA, 0x0C722C92, 0x2161E80A, 0x213CE6C0, 0x3348299B,
    0x153615F2, 0x116320F5, 0x2E6DEA46, 0x06732F1B, 0x2DD79792, 0x0F72C0C5,
    0x155A1592, 0x0E781134, 0x35238AD0, 0x0113EC90, 0x0CDDCEC8, 0x2415C6EF,
    0x3639A513, 0x3724EA62, 0x1B95FA0E, 0x0FF71A44, 0x1F105EE1, 0x16179AA2,
    0x1D13081C, 0x0F2E1BE4, 0x075D31A9, 0x2EF68287, 0x1A0F2D2A, 0x15E25EAE,
    0x235CB4B5, 0x36C4F36B, 0x1330A771, 0x29BD41DD, 0x0C89FC26, 0x0F4C5157,
    0x248F4F4D, 0x324556FE, 0x01D53CA8, 0x16FA5937, 0x19F374BA, 0x30F56D21,
    0x01F1BB95, 0x311F8BDF, 0x2709D399, 0x256A1427, 0x03D0BC56, 0x1A44C609,
    0x00372FDD, 0x1B21D418, 0x16E1A1FE, 0x1D316692, 0x1B3A10E6, 0x0B38289A,
    0x2DB57AE8, 0x07493573, 0x0B1D9A8C, 0x34D2E34C, 0x09ACDE2F, 0x29A2E956,
    0x0DA2260F, 0x192C7431, 0x2770234B, 0x307AF517, 0x1E7CDB9A, 0x1A0775FB,
    0x1463229C, 0x24EC8235, 0x21FB7C39, 0x11D636DD, 0x0379B1A1, 0x3B263545,
    0x351BD4A6, 0x2E40F4F0, 0x0D3C9A1F, 0x1200B6EB, 0x16E823DB, 0x06DB475A,
    0x29C7AC0C, 0x182CDEA9, 0x20879A8D, 0x32BC5990, 0x21E63437, 0x0BC696A5,
    0x060379E0, 0x05E231D5, 0x18E104DB, 0x04FC4FBD, 0x28CE7CFE, 0x2EB4545F,
    0x00E31C0F, 0x0C0AF907, 0x2D28AA4A, 0x0C878FC0, 0x1DFD0412, 0x1A6DC86F,
    0x0ACB2B8F, 0x12B390A2, 0x321896B2, 0x273B7284, 0x1871EC1D, 0x38ACC7A5,
    0x2E6F331B, 0x3ACADB35, 0x26E5E4D7, 0x3A8D2396,
};

// .rdata 0x141e44210: tiling offsets for 2 instances, 25 patterns x 2 (x, y).
constexpr float kTiling2[25][2][2] = {
    {{-24.0f, -19.9200001f}, {24.0f, -4.86000013f}},
    {{22.4599991f, -24.0f}, {14.8900003f, 24.0f}},
    {{23.9699993f, -24.0f}, {16.5799999f, 24.0f}},
    {{18.2700005f, -24.0f}, {-22.4099998f, 24.0f}},
    {{-7.32999992f, -24.0f}, {-1.76999998f, 24.0f}},
    {{-24.0f, 2.8599999f}, {24.0f, 1.02999997f}},
    {{-24.0f, -22.4699993f}, {24.0f, 21.5100002f}},
    {{-24.0f, -8.65999985f}, {24.0f, -1.96000004f}},
    {{6.98000002f, -24.0f}, {-10.4899998f, 24.0f}},
    {{-12.2299995f, -24.0f}, {0.319999993f, 24.0f}},
    {{20.4099998f, -24.0f}, {8.07999992f, 24.0f}},
    {{-24.0f, -15.3699999f}, {24.0f, 17.4500008f}},
    {{-23.5300007f, -24.0f}, {15.8500004f, 24.0f}},
    {{-1.32000005f, -24.0f}, {-16.1800003f, 24.0f}},
    {{3.74000001f, -24.0f}, {-23.9200001f, 24.0f}},
    {{-19.6000004f, -24.0f}, {-8.23999977f, 24.0f}},
    {{-24.0f, 19.5100002f}, {24.0f, -13.5699997f}},
    {{-24.0f, -4.69999981f}, {24.0f, -18.25f}},
    {{-24.0f, -18.0499992f}, {24.0f, -11.5900002f}},
    {{-13.1499996f, -24.0f}, {-12.8800001f, 24.0f}},
    {{-24.0f, -21.0f}, {24.0f, -10.04f}},
    {{7.28000021f, -24.0f}, {-2.78999996f, 24.0f}},
    {{15.2799997f, -24.0f}, {-12.46f, 24.0f}},
    {{-24.0f, 2.94000006f}, {24.0f, -23.8500004f}},
    {{20.3999996f, -24.0f}, {23.1700001f, 24.0f}},
};

// .rdata 0x141e443a0: tiling offsets for 3 instances, 25 patterns x 3 (x, y).
constexpr float kTiling3[25][3][2] = {
    {{-29.6800003f, -32.0f}, {38.6599998f, 0.0f}, {-20.6700001f, 32.0f}},
    {{-32.0f, 17.4099998f}, {0.0f, -16.1700001f}, {32.0f, 10.0699997f}},
    {{-23.1700001f, -32.0f}, {38.4700012f, 0.0f}, {-4.98999977f, 32.0f}},
    {{14.0500002f, -32.0f}, {-44.4300003f, 0.0f}, {-22.4799995f, 32.0f}},
    {{-32.0f, -24.2700005f}, {0.0f, 47.9500008f}, {32.0f, 17.75f}},
    {{-32.0f, 18.2299995f}, {0.0f, 37.3100014f}, {32.0f, 8.96000004f}},
    {{-5.55000019f, -32.0f}, {-15.96f, 0.0f}, {-9.57999992f, 32.0f}},
    {{-30.4899998f, -32.0f}, {-15.9399996f, 0.0f}, {31.5200005f, 32.0f}},
    {{-12.4899998f, -32.0f}, {-10.3100004f, 0.0f}, {-4.63999987f, 32.0f}},
    {{-32.0f, -23.9799995f}, {0.0f, -12.9300003f}, {32.0f, 3.9000001f}},
    {{27.5799999f, -32.0f}, {-36.4599991f, 0.0f}, {-18.4599991f, 32.0f}},
    {{19.0499992f, -32.0f}, {-12.4700003f, 0.0f}, {-29.5f, 32.0f}},
    {{-19.0f, -32.0f}, {-35.1100006f, 0.0f}, {-6.48000002f, 32.0f}},
    {{-32.0f, -6.5f}, {0.0f, 36.0200005f}, {32.0f, 29.9099998f}},
    {{-29.3899994f, -32.0f}, {-29.1800003f, 0.0f}, {27.5f, 32.0f}},
    {{-32.0f, -20.8099995f}, {0.0f, 11.6599998f}, {32.0f, -26.2800007f}},
    {{17.0900002f, -32.0f}, {-12.2700005f, 0.0f}, {4.30000019f, 32.0f}},
    {{-20.75f, -32.0f}, {-53.5499992f, 0.0f}, {-24.9500008f, 32.0f}},
    {{-32.0f, 20.1900005f}, {0.0f, 30.5699997f}, {32.0f, -12.9300003f}},
    {{-5.61000013f, -32.0f}, {-11.0600004f, 0.0f}, {2.6400001f, 32.0f}},
    {{-19.9599991f, -32.0f}, {-46.2000008f, 0.0f}, {16.6800003f, 32.0f}},
    {{-32.0f, 5.65999985f}, {0.0f, 13.9300003f}, {32.0f, 1.52999997f}},
    {{-3.68000007f, -32.0f}, {-42.7099991f, 0.0f}, {28.6900005f, 32.0f}},
    {{18.9099998f, -32.0f}, {57.2900009f, 0.0f}, {8.35999966f, 32.0f}},
    {{-17.0699997f, -32.0f}, {47.5800018f, 0.0f}, {10.7700005f, 32.0f}},
};

// .rdata 0x141e44600: tiling offsets for 4 instances, 25 patterns x 4 (x, y).
constexpr float kTiling4[25][4][2] = {
    {{-24.0f, 19.2999992f}, {-24.0f, -40.2000008f}, {24.0f, 45.6899986f}, {24.0f, 10.3400002f}},
    {{-12.8699999f, -24.0f}, {33.3499985f, 24.0f}, {-39.4000015f, -24.0f}, {-45.6199989f, 24.0f}},
    {{14.6499996f, -24.0f}, {-59.4399986f, 24.0f}, {64.2799988f, -24.0f}, {-10.4499998f, 24.0f}},
    {{-24.0f, -20.2600002f}, {-24.0f, 15.46f}, {24.0f, -0.99000001f}, {24.0f, -14.9399996f}},
    {{27.7299995f, -24.0f}, {35.5999985f, 24.0f}, {6.34000015f, -24.0f}, {-11.5200005f, 24.0f}},
    {{-0.119999997f, -24.0f}, {-16.3400002f, 24.0f}, {11.0600004f, -24.0f}, {14.1700001f, 24.0f}},
    {{-11.7799997f, -24.0f}, {-90.6999969f, 24.0f}, {58.0200005f, -24.0f}, {-36.2799988f, 24.0f}},
    {{47.8699989f, -24.0f}, {30.0f, 24.0f}, {17.3999996f, -24.0f}, {-24.3600006f, 24.0f}},
    {{-24.0f, 32.3600006f}, {-24.0f, 45.5200005f}, {24.0f, -52.9500008f}, {24.0f, -39.3800011f}},
    {{-24.0f, 29.3899994f}, {-24.0f, -23.2700005f}, {24.0f, -7.71999979f}, {24.0f, -31.3199997f}},
    {{-24.0f, 27.0699997f}, {-24.0f, -15.5600004f}, {24.0f, 22.9099998f}, {24.0f, -16.1900005f}},
    {{30.7000008f, -24.0f}, {5.46999979f, 24.0f}, {-16.4099998f, -24.0f}, {-47.7799988f, 24.0f}},
    {{-22.75f, -24.0f}, {19.6800003f, 24.0f}, {-41.8899994f, -24.0f}, {21.3899994f, 24.0f}},
    {{-4.32999992f, -24.0f}, {5.46999979f, 24.0f}, {-31.5900002f, -24.0f}, {5.80999994f, 24.0f}},
    {{-24.0f, 18.2900009f}, {-24.0f, 33.9399986f}, {24.0f, -67.2799988f}, {24.0f, -17.6200008f}},
    {{27.8099995f, -24.0f}, {-73.9899979f, 24.0f}, {85.2900009f, -24.0f}, {-3.24000001f, 24.0f}},
    {{29.9500008f, -24.0f}, {-40.0299988f, 24.0f}, {42.1300011f, -24.0f}, {-44.9900017f, 24.0f}},
    {{1.91999996f, -24.0f}, {13.7200003f, 24.0f}, {-3.24000001f, -24.0f}, {-5.75f, 24.0f}},
    {{-24.0f, 15.6999998f}, {-24.0f, 40.1599998f}, {24.0f, -25.7000008f}, {24.0f, 6.07000017f}},
    {{-24.0f, -0.49000001f}, {-24.0f, 45.9399986f}, {24.0f, -35.5999985f}, {24.0f, -17.2199993f}},
    {{-24.0f, -14.9700003f}, {-24.0f, 9.44999981f}, {24.0f, 7.61000013f}, {24.0f, -39.3300018f}},
    {{-24.0f, 17.0100002f}, {-24.0f, 80.7699966f}, {24.0f, -63.0800018f}, {24.0f, -2.1099999f}},
    {{45.1800003f, -24.0f}, {-69.0199966f, 24.0f}, {50.7299995f, -24.0f}, {9.36999989f, 24.0f}},
    {{-2.18000007f, -24.0f}, {15.9399996f, 24.0f}, {-4.67999983f, -24.0f}, {-16.5200005f, 24.0f}},
    {{44.1800003f, -24.0f}, {5.46999979f, 24.0f}, {-17.3999996f, -24.0f}, {-47.7799988f, 24.0f}},
};

// Constants, by their bit patterns in the exe.
constexpr float kHalfCell = std::bit_cast<float>(0x41C00000u);   // 24.0, .rdata 0x14192d3e8
constexpr float kCell = std::bit_cast<float>(0x42400000u);       // 48.0, 0x14193b12c
constexpr float kRandomZMin = std::bit_cast<float>(0x42F00000u); // 120.0, 0x141939310
constexpr float kDeg = std::bit_cast<float>(0x3C8EFA35u);        // pi / 180, 0x141ac6948
constexpr float kHalfPi = std::bit_cast<float>(0x3FC90FDBu);     // 0x14192d3d0
constexpr float kPi = std::bit_cast<float>(0x40490FDBu);         // rot.x for runtime flag 0x80 (0x140cc2af0)
constexpr float kTiltScale = std::bit_cast<float>(0x3EB33333u);  // 0.35, 0x14192d3c8
constexpr float kZoffMid = std::bit_cast<float>(0x42100000u);    // 36.0, 0x141ae3cd0
constexpr float kZoffDiv = std::bit_cast<float>(0x437F0000u);    // 255.0, 0x14192bb9c
constexpr float kZoffSpan = std::bit_cast<float>(0x42900000u);   // 72.0, 0x141afa698
constexpr float kScaleStep = std::bit_cast<float>(0x3D9E6666u);  // 0.07734375 (99/1280), 0x141e449c4
constexpr float kRngA = std::bit_cast<float>(0x30000000u);       // 2^-31, 0x1420b8620
constexpr float kRngB = std::bit_cast<float>(0x2FFFFFFFu);       // 4.6566126e-10, 0x1420b861c
constexpr double kPiD = 3.14159265358979323846;

// Runtime model-flag bits (after zone_runtime_flags) the expansion reads.
constexpr uint32_t kRtNoJitter = 0x1, kRtGroundAlign = 0xE, kRtSnap90 = 0x4, kRtTilt035 = 0x8,
                   kRtRandomTilt = 0x30, kRtNoTiltSign = 0x20, kRtRotXPi = 0x80, kRtYawFromNormal = 0x100,
                   kRtRandomZ = 0x4040, kRtWater = 0x8000, kRtSkipQuality0 = 0x100000;

/// The terrain as the measured ground query sees it: corner samples of the 96-unit quad.
struct Ground {
    const Terrain* t = nullptr;
    float X0 = 0, Y1 = 0, dx = 0, dy = 0;
    explicit Ground(const Terrain& terrain) {
        if (!terrain.present || terrain.chunks.empty() || terrain.chunks.front().samples < 2) return;
        const TerrainChunk& c0 = terrain.chunks.front();
        t = &terrain;
        X0 = c0.rect[0];
        Y1 = c0.rect[3];
        dx = (c0.rect[2] - c0.rect[0]) / float(c0.samples - 1);
        dy = (c0.rect[3] - c0.rect[1]) / float(c0.samples - 1);
    }
    /// Corner heights q = NW, NE, SW, SE and fractions fx (east), fy (south). False outside.
    bool quad(float x, float y, float q[4], float& fx, float& fy) const {
        if (!t) return false;
        const int i0 = (int)std::floor((x - X0) / dx), j0 = (int)std::floor((Y1 - y) / dy);
        bool in[4] = {};
        q[0] = terrain_height_at(*t, X0 + i0 * dx, Y1 - j0 * dy, &in[0]);
        q[1] = terrain_height_at(*t, X0 + (i0 + 1) * dx, Y1 - j0 * dy, &in[1]);
        q[2] = terrain_height_at(*t, X0 + i0 * dx, Y1 - (j0 + 1) * dy, &in[2]);
        q[3] = terrain_height_at(*t, X0 + (i0 + 1) * dx, Y1 - (j0 + 1) * dy, &in[3]);
        fx = (x - (X0 + i0 * dx)) / dx;
        fy = ((Y1 - j0 * dy) - y) / dy;
        return in[0] && in[1] && in[2] && in[3];
    }
    /// Height: the triangle of the NE-SW split that holds the point (measured, 4535 / 4535).
    bool height(float x, float y, float& h) const {
        float q[4], fx, fy;
        if (!quad(x, y, q, fx, fy)) return false;
        h = fx + fy <= 1 ? q[0] + (q[1] - q[0]) * fx + (q[2] - q[0]) * fy
                         : q[3] + (q[2] - q[3]) * (1 - fx) + (q[1] - q[3]) * (1 - fy);
        return true;
    }
    /// Normal (gx, gy, 1): the plane (NW, NE, SW) when fx <= fy, else (NE, SW, SE). Measured
    /// (3537 / 3537); not the height's triangle, and the query's code was not found.
    bool normal(float x, float y, float n[3]) const {
        float q[4], fx, fy;
        if (!quad(x, y, q, fx, fy)) return false;
        n[0] = fx <= fy ? (q[1] - q[0]) / dx : (q[3] - q[2]) / dx;
        n[1] = fx <= fy ? -(q[2] - q[0]) / dy : -(q[3] - q[1]) / dy;
        n[2] = 1;
        return true;
    }
};

/// What one expansion run counts (the UNPROVEN branches, for the warnings).
struct Counts {
    size_t tilingCells = 0, tilingInstances = 0, clamped = 0, water = 0, waterUnknown = 0, randomZ = 0,
           yawFromNormal = 0, snap90 = 0, subModel = 0, outsideTerrain = 0, tilingClampedT = 0, collideSkipped = 0;
};

struct Ctx {
    const WorldScene& s;
    const ZoneScatterRule& r;
    Ground ground;
    float rect[4];          ///< clamp rect: min x, min y, max x, max y
    bool hasWater = false;
    float waterZ = 0;
    Counts* counts;
};

/// 0x140cbc110: the slot's z offset, from its two `zOffsets` bytes.
float zone_z_offset(ZoneRng& g, const ZoneLayerModel& m) {
    const float r = g.fA();
    const float z0 = m.zOffsets[0];
    const float d = float(int(m.zOffsets[1]) - int(m.zOffsets[0]));
    return kZoffMid - ((z0 + r * d) / kZoffDiv) * kZoffSpan;
}

/// 0x140cc2880: jitter, clamp, ground height and the z rules. @p cd: the cell's collideData
/// triple (zoneFlags bit 1), else null.
void zone_place(const Ctx& c, ZoneRng& g, const float base[2], uint32_t rt, float zoff, const float* cd,
                float pos[3]) {
    float x = base[0], y = base[1];
    if (!(rt & kRtNoJitter)) {
        x = (kHalfCell - g.fB() * kCell) + base[0];
        y = (kHalfCell - g.fB() * kCell) + base[1];
    }
    // Clamp to the context rect (UNPROVEN: no collider reaches it).
    const float x1 = std::max(std::min(x, c.rect[2]), c.rect[0]);
    const float y1 = std::max(std::min(y, c.rect[3]), c.rect[1]);
    if (x1 != x || y1 != y) ++c.counts->clamped;
    x = x1;
    y = y1;
    float h = 0;
    const bool onTerrain = c.ground.height(x, y, h);
    if (!onTerrain) {
        h = 0;
        if (!cd) ++c.counts->outsideTerrain;
    }
    bool waterTaken = false;
    if (rt & kRtWater) {   // the water test (UNPROVEN): Z down, a ground below the water takes the water's value
        if (!c.hasWater) ++c.counts->waterUnknown;
        else if (!(h <= c.waterZ)) { h = c.waterZ; waterTaken = true; ++c.counts->water; }
    }
    if (!waterTaken) {
        if (cd) h = cd[2];
        else if ((rt & kRtRandomZ) == kRtRandomZ) {   // UNPROVEN: no collider takes it
            ++c.counts->randomZ;
            h = g.fB() * h;
            if (h - kRandomZMin < 0) h = kRandomZMin;
        }
    }
    pos[0] = x;
    pos[1] = y;
    pos[2] = h + zoff;
}

/// 0x140cc2af0: yaw, then random tilt or the ground-normal tilt.
void zone_rotate(const Ctx& c, ZoneRng& g, uint32_t rt, const ZoneLayer& L, const float pos[3], const float* cd,
                 float rot[3]) {
    rot[0] = (rt & kRtRotXPi) ? kPi : 0.0f;
    rot[1] = 0;
    rot[2] = (g.fB() * (L.rotRange[2][1] - L.rotRange[2][0]) + L.rotRange[2][0]) * kDeg;
    if (rt & kRtRandomTilt) {
        const bool sign = !(rt & kRtNoTiltSign);
        rot[0] = g.fB() * (L.rotRange[0][1] - L.rotRange[0][0]) + L.rotRange[0][0] + rot[0];
        float s = 1;
        if (sign && g.fB() > 0.5f) s = -1;
        rot[0] = rot[0] * kDeg * s;
        rot[1] = g.fB() * (L.rotRange[1][1] - L.rotRange[1][0]) + L.rotRange[1][0] + rot[1];
        s = 1;
        if (sign && g.fB() > 0.5f) s = -1;
        rot[1] = rot[1] * kDeg * s;
        return;
    }
    if (!(rt & kRtGroundAlign)) return;
    float n[3] = {0, 0, 1};
    if (cd) {
        n[0] = cd[0];
        n[1] = cd[1];
        const float t = (1.0f - n[0] * n[0]) - n[1] * n[1];
        n[2] = t < 0 ? 0.0f : std::sqrt(t);
    } else if (!c.ground.normal(pos[0], pos[1], n)) {
        n[0] = 0; n[1] = 0; n[2] = 1;
    }
    rot[0] = -std::atan2(n[1], std::sqrt(n[2] * n[2] + n[0] * n[0]));
    rot[1] = std::atan2(n[0], std::sqrt(n[2] * n[2] + n[1] * n[1]));
    if (rt & kRtYawFromNormal) {   // UNPROVEN: no collider takes it
        ++c.counts->yawFromNormal;
        rot[2] = -std::atan2(n[1], n[0]) - kHalfPi;
    }
    if (rt & kRtSnap90) {          // UNPROVEN: no collider takes it
        ++c.counts->snap90;
        if (std::fabs(n[0]) <= std::fabs(n[1])) rot[0] = n[1] <= 0 ? rot[0] + kHalfPi : rot[0] - kHalfPi;
        else rot[1] = n[0] <= 0 ? rot[1] - kHalfPi : rot[1] + kHalfPi;
    }
    if (rt & kRtTilt035) {
        rot[0] *= kTiltScale;
        rot[1] *= kTiltScale;
    }
}

/// The scale (0x140cc1f50 / 0x140cc1900): uniform in scaleRange, then round-to-even onto the
/// global 99/1280 step from scaleRange[0] (0x1409bf1f0).
float zone_scale(ZoneRng& g, const ZoneLayer& L) {
    const float s0 = L.scaleRange[0], s1 = L.scaleRange[1];
    const float s = g.fA() * (s1 - s0) + s0;
    const float q = std::nearbyint((s - s0) / kScaleStep);
    return q * kScaleStep + s0;
}

ZoneScatterDraw make_draw(uint32_t fileId, const float pos[3], const float rot[3], float scale) {
    ZoneScatterDraw d;
    d.fileId = fileId;
    for (int k = 0; k < 3; ++k) { d.pos[k] = pos[k]; d.rot[k] = rot[k]; }
    d.scale = scale;
    return d;
}

void exact_cell(const Ctx& c, uint32_t pi, const std::vector<uint32_t>& collideIndex,
                std::vector<ZoneScatterDraw>& out) {
    const ZoneScatter& z = c.s.zones;
    if (pi >= z.placements.size()) return;
    const ZonePlacement& p = z.placements[pi];
    if (p.layer >= z.layers.size() || p.zone >= z.zones.size()) return;
    const ZoneLayer& L = z.layers[p.layer];
    const uint32_t slot = (p.flag >> 1) & 7u;
    if (slot >= L.models.size()) return;
    const ZoneLayerModel& M = L.models[slot];
    const Zone& Z = z.zones[p.zone];

    const float* cd = nullptr;
    if (Z.zoneFlags & 2u) {   // collideData supplies the ground (0x140cc23c0)
        const uint32_t local = pi - Z.firstPlacement;
        const uint32_t ci = local < collideIndex.size() ? collideIndex[local] : UINT32_MAX;
        if (ci == UINT32_MAX || 3ull * ci + 2 >= Z.collideData.size()) { ++c.counts->collideSkipped; return; }
        cd = &Z.collideData[3ull * ci];
    }

    const uint32_t lx = uint32_t(p.c - Z.vertRect[0]), ly = uint32_t(p.r - Z.vertRect[1]);
    const uint32_t w = uint32_t(Z.vertRect[2] - Z.vertRect[0]);
    ZoneRng g(zone_cell_seed(Z.seed, lx, ly, w));
    float P[2];
    zone_anchor(c.s.bounds[0], c.s.bounds[1], p.c, p.r, P);   // context matrix: identity (UNPROVEN)
    const uint32_t rt = zone_runtime_flags(M.flags);
    const int quality = c.r.graphicsQuality;

    if (!(p.flag & 1u)) {   // single-instance path, 0x140cc1f50 (type 3; reproduces 4750 / 4750)
        const float zoff = zone_z_offset(g, M);
        float pos[3], rot[3];
        zone_place(c, g, P, rt, zoff, cd, pos);
        zone_rotate(c, g, rt, L, pos, cd, rot);
        const float scale = zone_scale(g, L);
        if (M.fileId) out.push_back(make_draw(M.fileId, pos, rot, scale));
        if (L.hasSubModel) {   // 0x140cbc210 (UNPROVEN: no collider exercises it)
            const float r = g.fA();
            const uint32_t srt = zone_runtime_flags(L.subModel.flags);
            if (r <= L.subModel.probability && (!(srt & kRtSkipQuality0) || quality != 0) && L.subModel.fileId) {
                ZoneScatterDraw d = make_draw(L.subModel.fileId, pos, rot, scale);
                d.subModel = true;
                out.push_back(d);
                ++c.counts->subModel;
            }
        }
        return;
    }

    // Tiling path, 0x140cc1900 (types 1-2; transcribed only, UNPROVEN).
    ++c.counts->tilingCells;
    const float zoff = zone_z_offset(g, M);
    if ((rt & kRtSkipQuality0) && quality == 0) return;
    uint32_t T = zone_tiling_max(L.tiling, quality);
    if (T > 4) { T = 4; ++c.counts->tilingClampedT; }   // the client asserts fillCount < MAX_TILING
    if (T < 1) { T = 1; ++c.counts->tilingClampedT; }   // irange(1, 0) would divide by zero in the client
    const uint32_t pattern = g.irange(0, 24);
    const uint32_t count = g.irange(1, T);
    static const float kZero[2] = {0, 0};
    for (uint32_t k = 0; k < count; ++k) {
        const float* off = count == 2 ? kTiling2[pattern][k] : count == 3 ? kTiling3[pattern][k]
                         : count == 4 ? kTiling4[pattern][k] : kZero;
        const uint32_t rtk = count > 1 ? (rt | kRtNoJitter) : rt;
        // groundcover is centred between four vertices: P + (24, 24) + offset
        const float base[2] = {(off[0] + P[0]) + kHalfCell, (off[1] + P[1]) + kHalfCell};
        float pos[3], rot[3];
        zone_place(c, g, base, rtk, zoff, cd, pos);
        zone_rotate(c, g, rtk, L, pos, cd, rot);
        const float scale = zone_scale(g, L);
        if (M.fileId) { out.push_back(make_draw(M.fileId, pos, rot, scale)); ++c.counts->tilingInstances; }
    }
}

Ctx make_ctx(const WorldScene& s, const ZoneScatterRule& r, Counts& counts) {
    Ctx c{s, r, Ground(s.terrain), {}, false, 0, &counts};
    c.rect[0] = s.bounds[0] + r.clampInset;
    c.rect[1] = s.bounds[1] + r.clampInset;
    c.rect[2] = s.bounds[2] - r.clampInset;
    c.rect[3] = s.bounds[3] - r.clampInset;
    // The water query's source is not located (UNPROVEN): the havk water surface, else the watr plane.
    if (s.water.hasHavkSurfaceZ) { c.hasWater = true; c.waterZ = s.water.havkSurfaceZ; }
    else if (s.water.hasPlane) { c.hasWater = true; c.waterZ = s.water.planeZ; }
    return c;
}

/// Model index of @p fileId in the shared table, appended when new.
uint32_t model_index(WorldScene& s, std::unordered_map<uint32_t, uint32_t>& modelOf, uint32_t fileId) {
    auto [it, added] = modelOf.try_emplace(fileId, uint32_t(s.models.size()));
    if (added) s.models.push_back(PropModel{fileId});
    return it->second;
}

} // namespace

uint32_t ZoneRng::next() {
    uint32_t u = state * 48271u + (state / 44488u) * 0x80000001u;   // (state / 44488) * -0x7FFFFFFF, uint32 wrap
    if (u > 0x7FFFFFFFu) u += 0x80000000u;
    if (u == 0) u = 0x075BD924u;
    state = u;
    return u;
}

float ZoneRng::fA() { return float(next()) * kRngA; }
float ZoneRng::fB() { return float(next()) * kRngB; }
uint32_t ZoneRng::irange(uint32_t a, uint32_t b) {
    const uint32_t n = next();
    return b < a ? a : a + n % (b - a + 1u);
}

uint32_t zone_cell_seed(uint8_t zoneSeed, uint32_t lx, uint32_t ly, uint32_t w) {
    return (uint32_t(zoneSeed) << 8) ^ kSeedTable[(lx + w * ly + lx * ly) & 255u] ^ (((ly * 4u) ^ lx) << 14);
}

uint32_t zone_runtime_flags(uint32_t pack) {
    // 0x141d6e110 (pack bit) -> 0x141d6e170 (runtime bit), 23 entries.
    static constexpr uint32_t kFrom[23] = {0x1,     0x2,      0x200000, 0x4,       0x8,       0x20000,
                                           0x10,    0x20,     0x200,    0x400000,  0x80,      0x100,
                                           0x400,   0x800,    0x1000,   0x2000,    0x10000,   0x40000,
                                           0x80000, 0x100000, 0x800000, 0x1000000, 0x2000000};
    static constexpr uint32_t kTo[23] = {0x1,     0x2,     0x4,      0x8,      0x10,     0x20,
                                         0x40,    0x80,    0x100,    0x200,    0x400,    0x800,
                                         0x2000,  0x4000,  0x8000,   0x10000,  0x80000,  0x1000,
                                         0x20000, 0x40000, 0x100000, 0x200000, 0x400000};
    uint32_t o = 0;
    for (int i = 0; i < 23; ++i)
        if (pack & kFrom[i]) o |= kTo[i];
    return o;   // (the layer-type-dependent 0x400000 bit is not read by the expansion)
}

uint32_t zone_tiling_max(uint8_t tiling, int quality) {
    const uint32_t t = tiling;
    if (t <= 1 || quality == 2) return t;
    if (quality == 1) return t == 4 ? 3u : t;
    if (quality == 0) return t == 4 ? 2u : t - 1;
    return t;
}

float zone_fade_quality_scale(int quality) {
    static constexpr float kScale[4] = {0.85f, 1.0f, 1.5f, 2.0f};   // 0x140cbc0b0 / 0x140cbc090 (research §1.3)
    return quality >= 0 && quality < 4 ? kScale[quality] : 1.0f;
}

std::vector<uint32_t> zone_collide_index(const ZoneScatter& z, uint32_t zone) {
    std::vector<uint32_t> out;
    if (zone >= z.zones.size()) return out;
    const Zone& Z = z.zones[zone];
    out.assign(Z.placementCount, UINT32_MAX);
    const int32_t* vr = Z.vertRect;
    if (vr[2] <= vr[0] || vr[3] <= vr[1]) return out;
    const int32_t cx0 = vr[0] >> 5, cy0 = vr[1] >> 5, cx1 = ((vr[2] - 1) >> 5) + 1;
    // Placements are in cell order (rows, then columns), so a stable sort by chunk keeps
    // each chunk's cells in rows-then-columns order.
    std::vector<std::pair<uint32_t, uint32_t>> byChunk;   // chunk, local placement
    byChunk.reserve(Z.placementCount);
    for (uint32_t i = 0; i < Z.placementCount; ++i) {
        const ZonePlacement& p = z.placements[Z.firstPlacement + i];
        byChunk.emplace_back(uint32_t((p.c >> 5) - cx0) + uint32_t((p.r >> 5) - cy0) * uint32_t(cx1 - cx0), i);
    }
    std::stable_sort(byChunk.begin(), byChunk.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    uint32_t chunk = UINT32_MAX, n = 0;
    for (const auto& [ci, i] : byChunk) {
        if (ci != chunk) { chunk = ci; n = 0; }
        if (ci < Z.offsetData.size()) out[i] = uint32_t(Z.offsetData[ci]) + n;
        ++n;
    }
    return out;
}

ZoneScatterRule zone_scatter_rule_in_force() {
    // docs/research/gw2-terrain-to-unity.md §1.3, "2026-10-16: zone scatter rule": the client's
    // rule reproduces 4750 / 4750 colliders (type 3); rulings S3 / S4.
    ZoneScatterRule r;
    r.mode = ZoneScatterMode::Exact;
    r.graphicsQuality = 2;
    r.clampInset = 1.0f;
    return r;
}

void zone_scatter_exact(const WorldScene& s, uint32_t placement, const ZoneScatterRule& r,
                        const std::vector<uint32_t>& collideIndex, std::vector<ZoneScatterDraw>& out) {
    Counts counts;
    const Ctx c = make_ctx(s, r, counts);
    exact_cell(c, placement, collideIndex, out);
}

double zone_unit_draw(uint64_t& state) {
    return double(splitmix64_next(state) >> 11) * 0x1.0p-53;
}

size_t scatter_instances(WorldScene& s, const ZoneScatterRule& r) {
    ZoneScatter& z = s.zones;
    z.mode = r.mode;
    if (r.mode == ZoneScatterMode::Colliders || !z.present || !s.hasBounds) return 0;
    std::unordered_map<uint32_t, uint32_t> modelOf;
    for (uint32_t m = 0; m < s.models.size(); ++m) modelOf.try_emplace(s.models[m].fileId, m);
    const size_t before = z.instances.size();
    std::vector<std::string>& warn = s.warnings;

    auto add = [&](uint32_t pi, uint32_t fileId, const float pos[3], const float rot[3], float scale,
                   ZoneSource src) {
        ZoneInstance inst;
        inst.model = model_index(s, modelOf, fileId);
        inst.layer = z.placements[pi].layer;
        inst.placement = pi;
        inst.source = src;
        client_world_matrix(pos, rot, scale, inst.world);
        z.instances.push_back(inst);
    };

    if (r.mode == ZoneScatterMode::Exact) {
        Counts counts;
        const Ctx c = make_ctx(s, r, counts);
        std::vector<ZoneScatterDraw> draws;
        size_t collided = 0;
        for (uint32_t zi = 0; zi < z.zones.size(); ++zi) {
            const Zone& Z = z.zones[zi];
            const std::vector<uint32_t> ci = (Z.zoneFlags & 2u) ? zone_collide_index(z, zi) : std::vector<uint32_t>();
            for (uint32_t i = 0; i < Z.placementCount; ++i) {
                const uint32_t pi = Z.firstPlacement + i;
                if (z.placements[pi].collider != UINT32_MAX) { ++collided; continue; }   // D5 step 5: never doubled
                draws.clear();
                exact_cell(c, pi, ci, draws);
                for (const ZoneScatterDraw& d : draws) add(pi, d.fileId, d.pos, d.rot, d.scale, ZoneSource::Scatter);
            }
        }
        const size_t added = z.instances.size() - before;
        warn.push_back("zone: zoneScatter = exact: " + std::to_string(added) +
                       " Scatter instances by the client's rule (Gw2-64.exe 0x140cc23c0; "
                       "docs/research/gw2-terrain-to-unity.md \xc2\xa7" "1.3, 2026-10-16); " +
                       std::to_string(collided) + " collided placements keep their Collider instance only");
        warn.push_back("zone: UNPROVEN: the tiling path (flag bit 0, layer types 1-2; 0x140cc1900) is transcribed "
                       "with no collider check: " + std::to_string(counts.tilingInstances) + " instances from " +
                       std::to_string(counts.tilingCells) + " cells, graphics quality " +
                       std::to_string(r.graphicsQuality) +
                       " (a setting; getter 0x140c58c10 not read), groundcover centred at the vertex + (24, 24)");
        warn.push_back("zone: UNPROVEN branches (transcribed; none of the 4750 colliders takes them), instances "
                       "taking each: clamp " + std::to_string(counts.clamped) + ", water " +
                       std::to_string(counts.water) + ", random z " + std::to_string(counts.randomZ) +
                       ", yaw from normal " + std::to_string(counts.yawFromNormal) + ", 90\xc2\xb0 snap " +
                       std::to_string(counts.snap90) + ", subModel " + std::to_string(counts.subModel) +
                       "; the zone context matrix is taken as identity");
        if (counts.waterUnknown)
            warn.push_back("zone: " + std::to_string(counts.waterUnknown) +
                           " instances of water-tested models (pack flag 0x1000) on a map with no water height; "
                           "the water test is skipped for them (UNPROVEN)");
        if (counts.outsideTerrain)
            warn.push_back("zone: " + std::to_string(counts.outsideTerrain) +
                           " Scatter instances lie outside the terrain; their ground height is 0");
        if (counts.tilingClampedT)
            warn.push_back("zone: " + std::to_string(counts.tilingClampedT) +
                           " tiling cells have a layer tiling outside 1-4 (the client asserts); clamped");
        if (counts.collideSkipped)
            warn.push_back("zone: " + std::to_string(counts.collideSkipped) +
                           " cells of collideData zones have no collideData entry; skipped, as the client does");
    } else {   // DensityMatched: castlemist's own rule (spec D5 step 4), UNPROVEN
        for (uint32_t pi = 0; pi < z.placements.size(); ++pi) {
            const ZonePlacement& p = z.placements[pi];
            if (p.collider != UINT32_MAX || p.layer >= z.layers.size()) continue;
            const ZoneLayer& L = z.layers[p.layer];
            const uint32_t slot = (p.flag >> 1) & 7u;
            if (slot >= L.models.size() || !L.models[slot].fileId) continue;
            uint64_t st = (uint64_t(s.mapFileId) << 32) ^ pi;
            const double u = -24.0 + 48.0 * zone_unit_draw(st);
            const double v = -24.0 + 48.0 * zone_unit_draw(st);
            const double yaw = L.rotRange[2][0] + zone_unit_draw(st) * (double(L.rotRange[2][1]) - L.rotRange[2][0]);
            const double sc = L.scaleRange[0] + zone_unit_draw(st) * (double(L.scaleRange[1]) - L.scaleRange[0]);
            float a[2];
            zone_anchor(s.bounds[0], s.bounds[1], p.c, p.r, a);
            float pos[3] = {float(a[0] + u), float(a[1] + v), 0};
            bool in = false;
            const float h = r.zFromTerrain ? terrain_height_at(s.terrain, pos[0], pos[1], &in) : 0.0f;
            pos[2] = in ? h : z.zones[p.zone].zPos;
            float rot[3] = {0, 0, float(yaw * kPiD / 180.0)};
            if (r.tiltFromGround) {   // D5 step 3's hypothesis as phrased: the normal, clamped to rotRange x/y
                bool a1 = false, b1 = false, c1 = false, d1 = false;
                const float zx1 = terrain_height_at(s.terrain, pos[0] + 1, pos[1], &a1);
                const float zx0 = terrain_height_at(s.terrain, pos[0] - 1, pos[1], &b1);
                const float zy1 = terrain_height_at(s.terrain, pos[0], pos[1] + 1, &c1);
                const float zy0 = terrain_height_at(s.terrain, pos[0], pos[1] - 1, &d1);
                if (a1 && b1 && c1 && d1) {
                    const double gx = (zx1 - zx0) / 2.0, gy = (zy1 - zy0) / 2.0;
                    const double t[2] = {-std::atan2(gy, std::sqrt(1 + gx * gx)),
                                         std::atan2(gx, std::sqrt(1 + gy * gy))};
                    for (int k = 0; k < 2; ++k)
                        rot[k] = float(std::clamp(t[k], L.rotRange[k][0] * kPiD / 180.0, L.rotRange[k][1] * kPiD / 180.0));
                }
            }
            add(pi, L.models[slot].fileId, pos, rot, float(sc), ZoneSource::Density);
        }
        warn.push_back("zone: zoneScatter = densityMatched is UNPROVEN: " +
                       std::to_string(z.instances.size() - before) +
                       " Density instances by castlemist's own rule (spec D5 step 4: u, v uniform in [-24, 24), yaw "
                       "uniform in rotRange.z, scale uniform in scaleRange, splitmix64 draws)");
    }

    // ZoneLayerModel::model for every slot (and subModel) an instance uses.
    std::unordered_map<uint32_t, uint32_t> used;
    for (const ZoneInstance& i : z.instances)
        if (i.model < s.models.size()) used.emplace(s.models[i.model].fileId, i.model);
    for (ZoneLayer& L : z.layers) {
        for (ZoneLayerModel& M : L.models) {
            const auto it = used.find(M.fileId);
            if (it != used.end()) M.model = it->second;
        }
        if (L.hasSubModel) {
            const auto it = used.find(L.subModel.fileId);
            if (it != used.end()) L.subModel.model = it->second;
        }
    }
    return z.instances.size() - before;
}

} // namespace castlemist::world
