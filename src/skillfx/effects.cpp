#include "castlemist/skillfx/effects.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

namespace castlemist::skillfx {

namespace {

bool slug_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
}

// Two groups of five slug characters joined by a dot, e.g. "Bp5rI.EWIp2" (the same
// shape content_map.cpp's looks_like_content_slug accepts).
bool is_slug(const std::string& s) {
    if (s.size() != 11 || s[5] != '.') return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != 5 && !slug_char(s[i])) return false;
    return true;
}

void add_unique(std::vector<uint32_t>& v, uint32_t id) {
    if (std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
}

}  // namespace

EffectClass effect_class_of_kind(uint32_t kind) {
    switch (kind) {
        case 0: return EffectClass::Audio;
        case 1: case 11: return EffectClass::Compound;
        case 2: return EffectClass::Decal;
        case 3: case 5: return EffectClass::Material;
        case 4: case 6: case 10: return EffectClass::Model;
        case 7: return EffectClass::PostProc;
        case 8: case 9: return EffectClass::ShakeSpring;
        default: return EffectClass::Unknown;
    }
}

const char* effect_class_name(EffectClass c) {
    switch (c) {
        case EffectClass::Audio: return "audio";
        case EffectClass::Compound: return "compound";
        case EffectClass::Decal: return "decal";
        case EffectClass::Material: return "material";
        case EffectClass::Model: return "model";
        case EffectClass::PostProc: return "postproc";
        case EffectClass::ShakeSpring: return "shakespring";
        case EffectClass::Unknown: break;
    }
    return "unknown";
}

bool is_compound_kind(uint32_t kind) { return effect_class_of_kind(kind) == EffectClass::Compound; }

std::string codename(const cntc::Object& o) {
    for (const auto& [field, s] : o.strings)
        if (is_slug(s)) return s;
    return {};
}

Guid guid_of(const cntc::Pack& p, const cntc::Object& o) {
    Guid g{};
    if (static_cast<uint64_t>(o.begin) + g.size() <= p.content.size())
        std::memcpy(g.data(), p.content.data() + o.begin, g.size());
    return g;
}

Effect read_effect(const cntc::PackSet& s, size_t pack, const cntc::Object& o, const ContainerOf& container) {
    Effect e;
    e.kind = o.data_id;
    e.cls = effect_class_of_kind(o.data_id);
    e.codename = codename(o);
    e.guid = guid_of(s.packs[pack], o);
    const std::vector<uint32_t>& refs = s.shared_file_refs();
    for (const auto& [field, idx] : o.files) {
        const uint32_t fid = idx < refs.size() ? refs[idx] : 0;
        if (!fid) continue;
        const std::string c = container ? container(fid) : std::string();
        if (c == "MODL" && !e.model) e.model = fid;
        if (c == "AMSP" && !e.sound) e.sound = fid;
    }
    return e;
}

std::vector<EffectRow> effect_rows(const cntc::PackSet& s, const ContainerOf& container) {
    std::vector<EffectRow> rows;
    for (size_t pi = 0; pi < s.packs.size(); ++pi) {
        const cntc::Pack& p = s.packs[pi];
        for (const cntc::Object& o : p.objects) {
            if (o.type != kEffectType) continue;
            const Effect e = read_effect(s, pi, o, container);
            EffectRow r;
            r.uid = p.u32(o, 20);
            r.kind = e.kind;
            r.model = e.model;
            r.sound = e.sound;
            r.codename = e.codename;
            for (const cntc::Ref& ref : o.refs) {
                auto [tp, to] = s.resolve(pi, ref);
                if (tp && to && to->type == kEffectType) ++r.children;
            }
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

SkillLinks find_effects(const cntc::PackSet& s, size_t pack, const cntc::Object& skill, const ContainerOf& container) {
    SkillLinks out;
    std::set<std::pair<const cntc::Pack*, uint32_t>> seen;
    // A compound effect (kind 1 or 11) is a group: its type-183 targets replace it, recursively.
    std::function<void(size_t, const cntc::Object&, uint32_t)> add = [&](size_t pi, const cntc::Object& o, uint32_t field) {
        if (!seen.insert({&s.packs[pi], o.begin}).second) return;
        if (!is_compound_kind(o.data_id)) {
            Effect e = read_effect(s, pi, o, container);
            e.field = field;
            out.effects.push_back(std::move(e));
            return;
        }
        for (const cntc::Ref& r : o.refs) {
            auto [tp, to] = s.resolve(pi, r);
            if (tp && to && to->type == kEffectType) add(static_cast<size_t>(tp - s.packs.data()), *to, field);
        }
    };
    for (const cntc::Ref& r : skill.refs) {
        auto [tp, to] = s.resolve(pack, r);
        if (!tp || !to) continue;
        if (to->type == kEffectType) add(static_cast<size_t>(tp - s.packs.data()), *to, r.field);
        else if (to->type == kSkillType && to->data_id < kFirstOwnSkillId) add_unique(out.applies, to->data_id);
        else if (to->type == kSkillType) add_unique(out.related_skills, to->data_id);
    }
    return out;
}

}  // namespace castlemist::skillfx
