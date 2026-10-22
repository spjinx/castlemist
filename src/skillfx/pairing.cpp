#include "castlemist/skillfx/pairing.h"

#include <algorithm>
#include <regex>
#include <tuple>
#include <utility>

namespace castlemist::skillfx {

namespace {

constexpr uint64_t kRepeatMs = 500;

bool is_effect_event(uint8_t sc) {
    return sc == evtc::kEffectGround || sc == evtc::kEffectAgent || sc == evtc::kEffectOld1 || sc == evtc::kEffectOld2;
}

}  // namespace

std::vector<Pairing> pair_effects(const std::vector<EvtcLog>& logs, uint32_t elite_spec,
                                  const std::set<uint32_t>& marker_buffs, uint32_t window_ms, uint32_t min_count) {
    struct Marker {
        uint64_t time;
        uint32_t skill;
        bool cast;  // ANIMATIONSTART (else a buff application)
    };
    struct Occurrence {
        Guid effect;
        std::vector<Marker> candidates;  // markers within the window before it
    };
    std::vector<Occurrence> occ;
    std::map<uint32_t, std::string> names;
    for (const EvtcLog& log : logs) {
        std::set<uint64_t> player;
        for (const EvtcAgent& a : log.agents)
            if (a.elite == elite_spec) player.insert(a.addr);
        const std::map<uint32_t, Guid> guids = effect_guids(log);
        for (const auto& [id, name] : log.skills)
            if (!name.empty()) names.emplace(static_cast<uint32_t>(id), name);

        // Per player: one player's casts never claim another's effects.
        std::map<uint64_t, std::vector<Marker>> markers;
        for (const EvtcEvent& e : log.events) {
            if (!player.count(e.src)) continue;
            if (e.statechange == evtc::kAnimationStart) markers[e.src].push_back({e.time, e.skill_id, true});
            else if (e.statechange == evtc::kBuffApply && marker_buffs.count(e.skill_id))
                markers[e.src].push_back({e.time, e.skill_id, false});
        }
        for (auto& [src, v] : markers)
            std::stable_sort(v.begin(), v.end(), [](const Marker& a, const Marker& b) { return a.time < b.time; });

        std::map<std::pair<uint64_t, Guid>, uint64_t> last;  // repeats of one effect closer than kRepeatMs are one use
        for (const EvtcEvent& e : log.events) {
            if (!is_effect_event(e.statechange) || !player.count(e.src)) continue;
            auto g = guids.find(e.skill_id);
            if (g == guids.end()) continue;
            auto l = last.find({e.src, g->second});
            if (l != last.end() && e.time - l->second < kRepeatMs) continue;
            last[{e.src, g->second}] = e.time;
            Occurrence o{g->second, {}};
            for (const Marker& m : markers[e.src])
                if (m.time <= e.time && e.time - m.time <= window_ms) o.candidates.push_back(m);
            if (!o.candidates.empty()) occ.push_back(std::move(o));
        }
    }
    // A skill's score for an effect: the occurrences it is a candidate for. Each occurrence goes to
    // its best-scoring candidate; ties go to a cast, then to the latest marker.
    std::map<std::pair<uint32_t, Guid>, uint32_t> score;
    for (const Occurrence& o : occ) {
        std::set<uint32_t> skills;
        for (const Marker& m : o.candidates) skills.insert(m.skill);
        for (uint32_t s : skills) ++score[{s, o.effect}];
    }
    std::map<std::pair<uint32_t, Guid>, uint32_t> counts;
    for (const Occurrence& o : occ) {
        const Marker* best = nullptr;
        for (const Marker& m : o.candidates) {
            if (!best) {
                best = &m;
                continue;
            }
            const auto key = [&](const Marker& x) { return std::make_tuple(score[{x.skill, o.effect}], x.cast, x.time); };
            if (key(m) > key(*best)) best = &m;
        }
        ++counts[{best->skill, o.effect}];
    }
    std::vector<Pairing> out;
    for (const auto& [key, n] : counts) {
        if (n < min_count) continue;
        auto nm = names.find(key.first);
        out.push_back({key.first, nm == names.end() ? std::string() : nm->second, key.second, n});
    }
    std::stable_sort(out.begin(), out.end(), [](const Pairing& a, const Pairing& b) {
        return std::tie(a.skill_id, b.count) < std::tie(b.skill_id, a.count);
    });
    return out;
}

std::map<Guid, std::string> read_ei_guids(std::istream& cs) {
    static const std::regex line(R"re(Guid\s+(\w+)\s*=\s*new\(\s*"([0-9A-Fa-f]{32})"\s*\))re");
    std::map<Guid, std::string> out;
    std::string s;
    while (std::getline(cs, s)) {
        std::smatch m;
        if (!std::regex_search(s, m, line)) continue;
        const std::string hex = m[2].str();
        Guid g{};
        for (size_t i = 0; i < g.size(); ++i) g[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        out.emplace(g, m[1].str());
    }
    return out;
}

}  // namespace castlemist::skillfx
