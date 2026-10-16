#include "character.h"

#include "castlemist/extract/content_types.h"
#include "castlemist/npc/tokens.h"

#include <algorithm>
#include <regex>

namespace castlemist::npc::detail {

namespace {

bool is_race(const std::string& t) {
    static const std::regex kRace("^(human|charr|norn|sylvari|asura)(male|femal[a-z]*)$|^creature[a-z]{2,3}$");
    return !t.empty() && std::regex_match(t, kRace);
}

}  // namespace

CharacterFields read_character(const PackSet& set, size_t pack_index, const Object& obj) {
    CharacterFields f;
    f.id = obj.data_id;
    const Pack& pack = set.packs[pack_index];

    std::vector<Ref> refs = obj.refs;
    std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return a.field < b.field; });

    for (uint32_t slot : {192u, 200u, 208u}) {
        for (const Ref& r : refs) {
            if (r.field != slot) continue;
            auto [tp, to] = set.resolve(pack_index, r);
            if (tp && to && to->type == 35 && tp->u32(*to, 44) == 24) f.weapons.emplace_back(slot, to->data_id);
            break;
        }
    }
    for (const Ref& r : refs) {
        if (r.field != 120 && r.field != 168) continue;
        auto [tp, to] = set.resolve(pack_index, r);
        if (tp && to && to->type == 64) f.skills.push_back(to->data_id);
    }

    for (uint32_t off = 0; obj.end - obj.begin >= 8 && off <= obj.end - obj.begin - 8; off += 8) {
        std::string t = decode_token(pack.u64(obj, off));
        if (!is_race(t)) continue;
        if (off + 16 <= obj.end - obj.begin) {
            uint64_t look = pack.u64(obj, off + 8);
            if (look != 0) f.looks.emplace_back(t, look);
        }
        if (f.race.empty()) f.race = std::move(t);
    }

    std::string m = decode_token(pack.u64(obj, 88));
    if (m == "undead" || m == "ghost") f.modifier = std::move(m);

    std::vector<std::pair<uint32_t, std::string>> strs = obj.strings;
    std::stable_sort(strs.begin(), strs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& s : strs) {
        if (::is_content_slug(s.second)) {
            f.codename = s.second;
            break;
        }
    }
    return f;
}

}  // namespace castlemist::npc::detail
