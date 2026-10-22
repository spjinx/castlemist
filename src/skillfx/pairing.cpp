#include "castlemist/skillfx/pairing.h"

#include <algorithm>
#include <regex>
#include <tuple>
#include <utility>

namespace castlemist::skillfx {

namespace {

bool is_effect_event(uint8_t sc) {
    return sc == evtc::kEffectGround || sc == evtc::kEffectAgent || sc == evtc::kEffectOld1 || sc == evtc::kEffectOld2;
}

}  // namespace

std::vector<Pairing> pair_effects(const std::vector<EvtcLog>& logs, uint32_t elite_spec,
                                  const std::set<uint32_t>& marker_buffs, uint32_t window_ms, uint32_t min_count) {
    std::map<std::pair<uint32_t, Guid>, uint32_t> counts;
    std::map<uint32_t, std::string> names;
    for (const EvtcLog& log : logs) {
        std::set<uint64_t> player;
        for (const EvtcAgent& a : log.agents)
            if (a.elite == elite_spec) player.insert(a.addr);
        const std::map<uint32_t, Guid> guids = effect_guids(log);
        for (const auto& [id, name] : log.skills)
            if (!name.empty()) names.emplace(static_cast<uint32_t>(id), name);

        std::vector<std::pair<uint64_t, uint32_t>> markers;  // {time, skill}
        for (const EvtcEvent& e : log.events) {
            if (!player.count(e.src)) continue;
            if (e.statechange == evtc::kAnimationStart ||
                (e.statechange == evtc::kBuffApply && marker_buffs.count(e.skill_id)))
                markers.push_back({e.time, e.skill_id});
        }
        std::stable_sort(markers.begin(), markers.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });

        std::set<std::pair<size_t, Guid>> seen;  // {marker index, effect}: a marker counts once per effect
        for (const EvtcEvent& e : log.events) {
            if (!is_effect_event(e.statechange) || !player.count(e.src)) continue;
            auto g = guids.find(e.skill_id);
            if (g == guids.end()) continue;
            auto m = std::upper_bound(markers.begin(), markers.end(), e.time,
                                      [](uint64_t t, const auto& mk) { return t < mk.first; });
            if (m == markers.begin()) continue;
            --m;
            if (e.time - m->first > window_ms) continue;
            if (seen.insert({static_cast<size_t>(m - markers.begin()), g->second}).second)
                ++counts[{m->second, g->second}];
        }
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
