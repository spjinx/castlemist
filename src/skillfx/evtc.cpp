#include "castlemist/skillfx/evtc.h"

#include <cstring>

namespace castlemist::skillfx {

namespace {

constexpr size_t kHeader = 16, kAgent = 96, kSkill = 68, kEvent = 64;

struct Reader {
    std::span<const uint8_t> d;
    template <typename T>
    T at(size_t p) const {
        T v{};
        if (p + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + p, sizeof(T));
        return v;
    }
    std::string text(size_t p, size_t n) const {
        std::string s;
        for (size_t i = 0; i < n && p + i < d.size() && d[p + i]; ++i) s += static_cast<char>(d[p + i]);
        return s;
    }
};

}  // namespace

std::optional<EvtcLog> read_evtc(std::span<const uint8_t> file) {
    std::vector<uint8_t> unzipped;
    if (file.size() >= 2 && file[0] == 'P' && file[1] == 'K') {
        unzipped = unzip_first(file);
        file = unzipped;
    }
    if (file.size() < kHeader || std::memcmp(file.data(), "EVTC", 4) != 0) return std::nullopt;
    const Reader r{file};
    EvtcLog log;
    log.build = r.text(4, 8);
    log.revision = r.at<uint8_t>(12);
    log.boss = r.at<uint16_t>(13);
    size_t p = kHeader;
    const uint32_t agents = r.at<uint32_t>(p);
    p += 4;
    if ((file.size() - p) / kAgent < agents) return std::nullopt;
    for (uint32_t i = 0; i < agents; ++i, p += kAgent)
        log.agents.push_back({r.at<uint64_t>(p), r.at<uint32_t>(p + 8), r.at<uint32_t>(p + 12), r.text(p + 28, 64)});
    const uint32_t skills = r.at<uint32_t>(p);
    p += 4;
    if ((file.size() - p) / kSkill < skills) return std::nullopt;
    for (uint32_t i = 0; i < skills; ++i, p += kSkill) log.skills[r.at<int32_t>(p)] = r.text(p + 4, 64);
    for (; p + kEvent <= file.size(); p += kEvent) {  // a trailing partial event is dropped
        EvtcEvent e;
        e.time = r.at<uint64_t>(p);
        e.src = r.at<uint64_t>(p + 8);
        e.dst = r.at<uint64_t>(p + 16);
        e.value = r.at<int32_t>(p + 24);
        e.buff_dmg = r.at<int32_t>(p + 28);
        e.overstack = r.at<uint32_t>(p + 32);
        e.skill_id = r.at<uint32_t>(p + 36);
        e.src_instid = r.at<uint16_t>(p + 40);
        e.dst_instid = r.at<uint16_t>(p + 42);
        e.src_master = r.at<uint16_t>(p + 44);
        e.dst_master = r.at<uint16_t>(p + 46);
        uint8_t* b[] = {&e.iff, &e.buff, &e.result, &e.is_activation, &e.is_buffremove, &e.is_ninety,
                        &e.is_fifty, &e.is_moving, &e.statechange, &e.is_flanking, &e.is_shields, &e.is_offcycle};
        for (size_t k = 0; k < std::size(b); ++k) *b[k] = file[p + 48 + k];
        log.events.push_back(e);
    }
    return log;
}

std::map<uint32_t, Guid> effect_guids(const EvtcLog& log) {
    std::map<uint32_t, Guid> out;
    for (const EvtcEvent& e : log.events) {
        if (e.statechange != evtc::kIdToGuid) continue;
        Guid g{};
        std::memcpy(g.data(), &e.src, 8);
        std::memcpy(g.data() + 8, &e.dst, 8);
        out[e.skill_id] = g;
    }
    return out;
}

}  // namespace castlemist::skillfx
