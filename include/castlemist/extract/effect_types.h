/// @file
/// @brief CPU mirrors of the MODL effect data added for the particle browser
///        (docs/superpowers/specs/2026-10-28-effect-decode-design.md).
/// @ingroup extract
///
/// Like model_types.h, these live in the global namespace.
///
/// Fields keep the template's names and stored values: `{min, max}` pairs as the
/// file holds them, packed colours as dwords. What a value means is in
/// docs/research/gw2-particle-system.md; an unproven one stays raw.

#pragma once

#include <cstdint>
#include <vector>

#include "castlemist/native/granny_anim.hpp"

/// @brief `MeshEmitterSettingsV*`, whole. `present` = the emitter's pointer is set.
struct MeshEmitterCPU {
    bool present = false;
    uint32_t fileId = 0;           ///< The particle mesh (MODL fileId).
    uint32_t alignment = 0;
    float bankThreshold = 0, bankScale = 0, obstacleRadius = 0;
    float rotation[3][2] = {}, rotationDrag[3] = {}, scaleChange[3][2] = {}, scaleInitial[3][2] = {}, spin[3][2] = {};
};

/// @brief `ModelParticleEmitterV70` fields older versions lack. `present` = the version has them.
struct ParticleEmitterExtraCPU {
    bool present = false;
    float accelerationDistRange[2] = {0, 0}, accelerationDistSpeed[2] = {0, 0};
    float velocityDistRange[2] = {0, 0}, velocityDistSpeed[2] = {0, 0};
    float velocityInherit[2] = {0, 0};
    uint32_t affinity = 0;
    MeshEmitterCPU mesh;
};

/// @brief `ModelStreakAnchorV*`.
struct StreakAnchorCPU {
    int boneJoint = -1;            ///< -1 = not in the skeleton (or no bone).
    uint64_t bone = 0;
    uint32_t colorStart = 0, colorEnd = 0;   ///< Packed colours, as stored.
    float falloff = 0, lifetime = 0, texV = 0;
    uint32_t flags = 0;
};

/// @brief `ModelStreakV*`: a ribbon through its anchors.
struct StreakCPU {
    int boneJoint = -1;
    uint64_t bone = 0;
    float acceleration[3] = {0, 0, 0}, velocity[3] = {0, 0, 0};
    std::vector<uint32_t> anchorIndices;     ///< Into ModelPreview::streakAnchors.
    uint32_t flags = 0, materialIndex = 0;
    float jitter = 0, noise = 0, spawnDist = 0, texScale = 0, wind = 0;
};

/// @brief `ModelObstacleDataV*`: a shape particles collide with.
struct ObstacleCPU {
    int boneJoint = -1;
    uint64_t bone = 0;
    uint32_t affinity = 0, flags = 0;
    uint8_t type = 0, response = 0;
    float dragCoef = 0, gravityCoef = 0, length = 0, width = 0, height = 0, radius = 0;
    float transform[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    bool hasTransform = false;
};

/// @brief `ModelEffectWindV*` (`ModelFileData.windData.effectWind`).
struct EffectWindCPU {
    int boneJoint = -1;
    uint64_t bone = 0;
    float farDistance = 0, intensity = 0, nearDistance = 0;
};

/// @brief What a clip effect track belongs to (the clip's list it came from).
///        The fx layer uses the same numbers (fx::effect_track_meaning).
enum class EffectTrackTarget : uint8_t { Cloud = 0, Light = 1, Streak = 2, Lightning = 3, Wind = 4 };

/// @brief One `ModelTrackTypeData` of a clip's cloudAnim / lightAnimData / streakAnim
///        (per anchor) / lightningAnim / windAnimData. `type` as stored.
struct EffectTrackCPU {
    EffectTrackTarget target = EffectTrackTarget::Cloud;
    uint64_t bone = 0;
    int boneJoint = -1;
    uint8_t type = 0;
    uint32_t trackGroup = 0, vectorTrack = 0;
    float initial[4] = {0, 0, 0, 0};
    bool hasTrack = false;                 ///< The granny vector track was found.
    castlemist::granny::Curve curve;       ///< That track (curve.dim floats), when hasTrack.
};
