#include "castlemist/world/stream.h"

#include <algorithm>
#include <limits>

namespace castlemist::world {

std::vector<uint32_t> model_load_order(const WorldScene& s, const float cam[3]) {
    const size_t n = s.models.size();
    std::vector<double> key(n, std::numeric_limits<double>::infinity());
    for (const PropInstance& p : s.props) {
        if (p.model >= n) continue;
        const double dx = double(p.world[12]) - cam[0];
        const double dy = double(p.world[13]) - cam[1];
        const double dz = double(p.world[14]) - cam[2];
        key[p.model] = std::min(key[p.model], dx * dx + dy * dy + dz * dz);
    }
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = uint32_t(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });
    return order;
}

} // namespace castlemist::world
