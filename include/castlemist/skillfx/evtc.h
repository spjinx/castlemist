/// @file
/// @brief Reader for arcdps combat logs (.evtc, and .zevtc = zipped), revision 1.
///
/// Layout from arcdps' EVTC readme (https://www.deltaconnected.com/arcdps/evtc/README.txt):
/// header ("EVTC", 8-char build date, revision byte, u16 boss id, pad), u32 agent count +
/// 96-byte agents, u32 skill count + 68-byte skills, then 64-byte combat events. The user
/// records these logs; castlemist only reads the files (it never touches the client).

#pragma once

#include "castlemist/skillfx/effects.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace castlemist::skillfx {

struct EvtcAgent {
    uint64_t addr = 0;
    uint32_t prof = 0, elite = 0;  ///< elite = specialization id (62 = Firebrand) for players.
    std::string name;
};

/// @brief One revision-1 combat event (64 bytes).
struct EvtcEvent {
    uint64_t time = 0, src = 0, dst = 0;
    int32_t value = 0, buff_dmg = 0;
    uint32_t overstack = 0, skill_id = 0;
    uint16_t src_instid = 0, dst_instid = 0, src_master = 0, dst_master = 0;
    uint8_t iff = 0, buff = 0, result = 0, is_activation = 0, is_buffremove = 0, is_ninety = 0, is_fifty = 0,
            is_moving = 0, statechange = 0, is_flanking = 0, is_shields = 0, is_offcycle = 0;
};

struct EvtcLog {
    std::string build;
    uint8_t revision = 0;
    uint16_t boss = 0;
    std::vector<EvtcAgent> agents;
    std::map<int32_t, std::string> skills;
    std::vector<EvtcEvent> events;
};

/// @brief The statechanges castlemist reads (arcdps enum cbtstatechange).
namespace evtc {
constexpr uint8_t kEffectOld1 = 45;      ///< CBTS_EFFECT1_DEFUNC (builds before 2023-07-16)
constexpr uint8_t kIdToGuid = 46;        ///< CBTS_IDTOGUID: content id -> GUID
constexpr uint8_t kEffectOld2 = 51;      ///< CBTS_EFFECT2_DEFUNC (builds before 2025-05-26)
constexpr uint8_t kEffectGround = 60;    ///< CBTS_EFFECTGROUNDCREATE
constexpr uint8_t kEffectAgent = 62;     ///< CBTS_EFFECTAGENTCREATE
constexpr uint8_t kAnimationStart = 67;  ///< CBTS_ANIMATIONSTART: skill_id = the cast
constexpr uint8_t kBuffApply = 69;       ///< CBTS_BUFFAPPLY
}  // namespace evtc

/// @brief Parses a log; a .zevtc's first zip entry is inflated first. nullopt if it isn't one.
std::optional<EvtcLog> read_evtc(std::span<const uint8_t> file);

/// @brief IDTOGUID events: content id (skill_id) -> GUID (src's 8 bytes then dst's, little-endian).
std::map<uint32_t, Guid> effect_guids(const EvtcLog& log);

/// @brief The first entry of a zip archive, inflated (stored or deflate); empty on failure.
std::vector<uint8_t> unzip_first(std::span<const uint8_t> zip);

}  // namespace castlemist::skillfx
