#include "castlemist/skillfx/skill_set.h"

#include <algorithm>
#include <set>

namespace castlemist::skillfx {

namespace {

constexpr uint32_t kItemBarField = 264;   // bundle item -> its type-65 skill bar
constexpr uint32_t kBarFirstSlot = 192;   // bar slots: +192 + 112*k, k = 0..5
constexpr uint32_t kBarSlotStride = 112;
constexpr int kBarSlots = 6;

std::string prefix_of(const std::string& slug) {
    const size_t dot = slug.find('.');
    return dot == std::string::npos ? std::string() : slug.substr(0, dot + 1);
}

// The prefix most of `slugs` share ("" when none has one).
std::string majority_prefix(const std::vector<std::string>& slugs) {
    std::map<std::string, size_t> n;
    for (const std::string& s : slugs)
        if (std::string p = prefix_of(s); !p.empty()) ++n[p];
    std::string best;
    size_t most = 0;
    for (const auto& [p, c] : n)
        if (c > most) {
            best = p;
            most = c;
        }
    return best;
}

size_t index_of(const cntc::PackSet& s, const cntc::Pack* p) { return static_cast<size_t>(p - s.packs.data()); }

}  // namespace

SkillSet build_skill_set(const cntc::PackSet& s, const std::vector<uint32_t>& seed_skill_ids,
                         const ContainerOf& container) {
    SkillSet out;
    const std::set<uint32_t> seeds(seed_skill_ids.begin(), seed_skill_ids.end());
    std::vector<std::string> seed_slugs;
    for (const cntc::Pack& p : s.packs)
        for (const cntc::Object& o : p.objects)
            if (o.type == kSkillType && seeds.count(o.data_id)) seed_slugs.push_back(codename(o));
    out.skill_namespace = majority_prefix(seed_slugs);
    if (out.skill_namespace.empty()) return out;

    std::map<uint32_t, size_t> by_id;  // dataId -> index in out.skills
    for (size_t pi = 0; pi < s.packs.size(); ++pi)
        for (const cntc::Object& o : s.packs[pi].objects) {
            if (o.type != kSkillType || by_id.count(o.data_id)) continue;
            std::string slug = codename(o);
            if (prefix_of(slug) != out.skill_namespace) continue;
            SkillEntry e;
            e.id = o.data_id;
            e.codename = std::move(slug);
            e.links = find_effects(s, pi, o, container);
            by_id[e.id] = out.skills.size();
            out.skills.push_back(std::move(e));
        }

    // Bundle items (tomes): item +264 -> bar; bar slots name the bundle's skills.
    for (size_t pi = 0; pi < s.packs.size(); ++pi)
        for (const cntc::Object& item : s.packs[pi].objects) {
            if (item.type != kItemType || prefix_of(codename(item)) != out.skill_namespace) continue;
            for (const cntc::Ref& r : item.refs) {
                if (r.field != kItemBarField) continue;
                auto [bp, bar] = s.resolve(pi, r);
                if (!bp || !bar || bar->type != kBarType) continue;
                for (const cntc::Ref& slot : bar->refs) {
                    if (slot.field < kBarFirstSlot || (slot.field - kBarFirstSlot) % kBarSlotStride) continue;
                    const int k = static_cast<int>((slot.field - kBarFirstSlot) / kBarSlotStride);
                    if (k >= kBarSlots) continue;
                    auto [sp, skill] = s.resolve(index_of(s, bp), slot);
                    if (!sp || !skill || skill->type != kSkillType) continue;
                    auto it = by_id.find(skill->data_id);
                    if (it == by_id.end()) continue;
                    out.skills[it->second].bundle_item = item.data_id;
                    out.skills[it->second].bundle_slot = k;
                }
            }
        }
    std::sort(out.skills.begin(), out.skills.end(), [](const SkillEntry& a, const SkillEntry& b) { return a.id < b.id; });

    std::vector<std::string> fx_slugs;
    std::set<Guid> linked;
    for (const SkillEntry& e : out.skills)
        for (const Effect& f : e.links.effects) {
            fx_slugs.push_back(f.codename);
            linked.insert(f.guid);
        }
    out.effect_namespace = majority_prefix(fx_slugs);
    if (out.effect_namespace.empty()) return out;
    for (size_t pi = 0; pi < s.packs.size(); ++pi)
        for (const cntc::Object& o : s.packs[pi].objects) {
            if (o.type != kEffectType || prefix_of(codename(o)) != out.effect_namespace) continue;
            Effect f = read_effect(s, pi, o, container);
            if (!linked.count(f.guid)) out.unlinked.push_back(std::move(f));
        }
    return out;
}

std::map<Guid, std::pair<size_t, const cntc::Object*>> effect_index(const cntc::PackSet& s) {
    std::map<Guid, std::pair<size_t, const cntc::Object*>> out;
    for (size_t pi = 0; pi < s.packs.size(); ++pi)
        for (const cntc::Object& o : s.packs[pi].objects)
            if (o.type == kEffectType) out.emplace(guid_of(s.packs[pi], o), std::make_pair(pi, &o));
    return out;
}

}  // namespace castlemist::skillfx
