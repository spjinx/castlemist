/// @file
/// @brief Stats as JSON (see stats.h).

#include "castlemist/fxsim/stats.h"

namespace castlemist::fxsim {

nlohmann::json stats_json(const Stats& s) {
    nlohmann::json em = nlohmann::json::array();
    for (const EmitterStats& e : s.emitters)
        em.push_back({{"spawned", e.spawned}, {"expired", e.expired}, {"live", e.live},
                      {"spread", {e.spread[0], e.spread[1], e.spread[2]}}, {"meanSpeed", e.meanSpeed},
                      {"alphaAt", std::vector<float>(e.alphaAt, e.alphaAt + 5)},
                      {"sizeAt", std::vector<float>(e.sizeAt, e.sizeAt + 5)}});
    return {{"live", s.live}, {"dropped", s.dropped}, {"emitters", em}};
}

}  // namespace castlemist::fxsim
