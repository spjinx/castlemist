#include "castlemist/world/stream.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace castlemist::world {

namespace {

/// Squared distance from @p cam to each model's nearest prop or zone instance (infinity
/// for a model with no instance).
std::vector<double> nearest_keys(const WorldScene& s, const float cam[3]) {
    const size_t n = s.models.size();
    std::vector<double> key(n, std::numeric_limits<double>::infinity());
    for (const PropInstance& p : s.props) {
        if (p.model >= n) continue;
        const double dx = double(p.world[12]) - cam[0];
        const double dy = double(p.world[13]) - cam[1];
        const double dz = double(p.world[14]) - cam[2];
        key[p.model] = std::min(key[p.model], dx * dx + dy * dy + dz * dz);
    }
    for (const ZoneInstance& z : s.zones.instances) {   // zone models rank by their zone instances too
        if (z.model >= n) continue;
        const double dx = double(z.world[12]) - cam[0];
        const double dy = double(z.world[13]) - cam[1];
        const double dz = double(z.world[14]) - cam[2];
        key[z.model] = std::min(key[z.model], dx * dx + dy * dy + dz * dz);
    }
    return key;
}

} // namespace

void rank_models(const WorldScene& s, const float cam[3], std::vector<uint32_t>& models) {
    const std::vector<double> key = nearest_keys(s, cam);
    const double inf = std::numeric_limits<double>::infinity();
    auto k = [&](uint32_t m) { return m < key.size() ? key[m] : inf; };
    std::sort(models.begin(), models.end(), [&](uint32_t a, uint32_t b) {
        const double ka = k(a), kb = k(b);
        if (ka != kb) return ka < kb;
        return a < b;
    });
}

std::vector<uint32_t> model_load_order(const WorldScene& s, const float cam[3]) {
    std::vector<uint32_t> order(s.models.size());
    std::iota(order.begin(), order.end(), 0u);
    rank_models(s, cam, order);
    return order;
}

ModelQueue::ModelQueue(const WorldScene& s, const float cam[3]) : s_(s), rest_(model_load_order(s, cam)) {}

bool ModelQueue::pop(uint32_t& model) {
    std::lock_guard<std::mutex> lock(m_);
    if (head_ >= rest_.size()) return false;
    model = rest_[head_++];
    return true;
}

void ModelQueue::reorder(const float cam[3]) {
    std::lock_guard<std::mutex> lock(m_);
    rest_.erase(rest_.begin(), rest_.begin() + static_cast<std::ptrdiff_t>(head_));
    head_ = 0;
    rank_models(s_, cam, rest_);
}

size_t ModelQueue::remaining() const {
    std::lock_guard<std::mutex> lock(m_);
    return rest_.size() - head_;
}

} // namespace castlemist::world
