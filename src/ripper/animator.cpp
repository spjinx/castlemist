#include "castlemist/ripper/animator.h"

#include "castlemist/db/index_db.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/ripper/character_export.h"
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <regex>
#include <set>
#include <unordered_map>

namespace castlemist::ripper {

namespace {

constexpr int kSlots = 12;  // token64 name packing: 12 five-bit letters, then a 4-bit variant

uint64_t slot(uint64_t token, int i) { return (token >> (5 * i)) & 0x1Full; }

// The template with empty slots shown as '_' (the decoded form the research note uses).
std::string template_name(uint64_t sequence) {
    std::string s;
    for (int i = 0; i < kSlots; ++i) {
        const uint64_t v = slot(sequence, i);
        s.push_back(v == 0 ? '_' : v <= 26 ? static_cast<char>('a' + v - 1) : '?');
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s;
}

}  // namespace

uint64_t fill_stance(uint64_t sequence, char stance) {
    if (slot(sequence, 0) != 0 || stance < 'a' || stance > 'z') return sequence;
    return sequence | static_cast<uint64_t>(stance - 'a' + 1);
}

bool sequence_matches(uint64_t sequence, uint64_t token) {
    for (int i = 0; i < kSlots; ++i) {
        const uint64_t want = slot(sequence, i);
        if (want != 0 && want != slot(token, i)) return false;
    }
    return true;
}

int find_clip(uint64_t sequence, const std::vector<uint64_t>& tokens, char stance, bool loops) {
    if (sequence == 0) return -1;
    // Within a name, the variant decides: 0 the plain clip, else 4 / 8 / 12 an intro / loop /
    // outro (as GW2's emotes are split). A looping state wants the loop part, a one-shot the intro.
    auto rank = [&](uint64_t token) {
        const uint64_t v = token >> 60;
        if (v == 0) return 0;
        if (v == (loops ? 8u : 4u)) return 1;
        if (v == (loops ? 4u : 8u)) return 2;
        return 3;
    };
    const bool open = slot(sequence, 0) == 0;
    const char tries[] = {stance, 'u', 0};
    for (char s : tries) {
        if (!open && s != 0 && s != stance) continue;  // a named stance only has one try
        const uint64_t want = s ? fill_stance(sequence, s) : sequence;
        int best = -1;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!sequence_matches(want, tokens[i])) continue;
            if (best < 0 || rank(tokens[i]) < rank(tokens[static_cast<size_t>(best)])) best = static_cast<int>(i);
        }
        if (best >= 0) return best;
    }
    return -1;
}

bool is_locomotion_state(const std::string& name) {
    static const std::regex re(
        "^(Ground (Idle|Alert|Fidget|Walk( .*)?|Run( .*)?|Sprint( .*)?|Turn (Left|Right))"
        "|Air (Jump|Jump Prepare|Fall)( (Forward|Backward|Left|Right)( (Left|Right))?)?"
        "|Water (Idle|Surface Idle|Fidget|Float|Sink|Swim( .*)?|Surface Swim( .*)?))$");
    return std::regex_match(name, re);
}

bool is_character_state(const std::string& name) { return name.rfind("Mount ", 0) != 0; }

std::string emote_label(const std::string& clip_name) {
    // The emotes a player has (the chat command's name), by clip; everything else the emote
    // table holds (NPC gestures, skill and gathering clips, mounted variants) is left out.
    static const std::pair<const char*, const char*> known[] = {
        {"ugeno", "No"}, {"ugeyes", "Yes"}, {"ugesit", "Sit"}, {"ugebow", "Bow"}, {"ugecry", "Cry"},
        {"ugewave", "Wave"}, {"ugerock", "Rock"}, {"ugetalk", "Talk"}, {"ugedance", "Dance"},
        {"ugeshrug", "Shrug"}, {"ugelaugh", "Laugh"}, {"ugekneel", "Kneel"}, {"ugesleep", "Sleep"},
        {"ugecheer", "Cheer"}, {"ugepaper", "Paper"}, {"ugescissor", "Scissors"}, {"ugecower", "Cower"},
        {"ugepoint", "Point"}, {"ugesalute", "Salute"}, {"ugebeckon", "Beckon"}, {"ugeponder", "Ponder"},
        {"ugethreat", "Threaten"}, {"ugesprised", "Surprised"}, {"clap", "Clap"}, {"eflex", "Flex"},
        {"shocked", "Shocked"}, {"whistle", "Whistle"}, {"eyeehaw", "Yee-haw"}, {"leanwall", "Lean on Wall"},
        {"thumbsup", "Thumbs Up"}, {"thumbsdown", "Thumbs Down"}, {"blowkiss", "Blow Kiss"},
        {"possessed", "Possessed"}, {"echefkiss", "Chef's Kiss"}, {"magictrick", "Magic Trick"},
        {"drinkwater", "Drink"}, {"goldenbless", "Bless"}, {"npcarmcross", "Cross Arms"},
        {"emote_stretch", "Stretch"},
    };
    std::string n = clean_clip_name(clip_name);
    while (!n.empty() && std::isdigit(static_cast<unsigned char>(n.back()))) n.pop_back();
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& [clip, label] : known)
        if (n == clip) return label;
    return {};
}

AnimatorPlan plan_animator(const model::AnimMachine& machine, const std::vector<uint64_t>& tokens, char stance,
                           const std::function<bool(const std::string&)>& keep) {
    using Action = model::AnimMachineAction;
    AnimatorPlan plan;
    plan.stance = stance;
    std::set<std::string> kept;
    std::set<int> used;
    std::set<std::string> missing;
    auto resolve = [&](uint64_t sequence, bool loops) {
        const int c = find_clip(sequence, tokens, stance, loops);
        if (c >= 0) used.insert(c);
        else if (sequence) missing.insert(template_name(sequence));
        return c;
    };

    for (const model::AnimMachineState& st : machine.states) {
        if (keep && !keep(st.name)) continue;
        AnimatorState s;
        s.name = st.name;
        for (const Action& a : st.actions) {
            if (a.kind == Action::PlayAnimation && a.sequence) {
                s.sequence = a.sequence;
                s.blendIn = a.blendInTime;
                s.flags = a.flags;
                s.clip = resolve(a.sequence, (a.flags & 0x10) == 0);
            } else if (a.kind == Action::SetRotation) {
                // a quaternion about +Z: (0, 0, sin(t/2), cos(t/2))
                s.yawDegrees = static_cast<float>(2.0 * std::atan2(a.rotation[2], a.rotation[3]) * 180.0 / 3.14159265358979);
            }
        }
        kept.insert(st.name);
        plan.states.push_back(std::move(s));
    }

    for (const model::AnimMachineState& st : machine.states) {
        if (!kept.count(st.name)) continue;
        for (const model::AnimMachineTransition& tr : st.transitions) {
            if (!kept.count(tr.targetStateName)) continue;
            AnimatorTransition t;
            t.from = st.name;
            t.to = tr.targetStateName;
            t.event = tr.name;
            for (const Action& a : tr.actions) {
                if (a.kind != Action::PlayAnimation && a.kind != Action::SetAnimationOptions) continue;
                t.blendIn = a.blendInTime;
                t.blendOut = a.blendOutTime;
                t.flags = a.flags;
                if (a.kind == Action::PlayAnimation && a.sequence) {
                    t.sequence = a.sequence;
                    t.clip = resolve(a.sequence, false);
                }
            }
            plan.transitions.push_back(std::move(t));
        }
    }

    plan.clips.assign(used.begin(), used.end());
    plan.missing.assign(missing.begin(), missing.end());
    return plan;
}

std::string clean_clip_name(const std::string& granny_name) {
    std::string out;
    bool gap = false;
    for (char c : granny_name) {
        if (c == ' ' || static_cast<unsigned char>(c) < 0x20) {
            gap = true;
            continue;
        }
        if (gap && !out.empty()) out.push_back('_');
        gap = false;
        out.push_back(c);
    }
    return out.empty() ? std::string("clip") : out;
}

void select_clips(ModelPreview& model, const std::vector<std::pair<uint64_t, std::string>>& keep) {
    std::unordered_map<uint64_t, std::string> names(keep.begin(), keep.end());
    ModelPreview kept;
    std::set<uint64_t> taken;
    for (size_t i = 0; i < model.animClips.size(); ++i) {
        const uint64_t tok = i < model.animClipTokens.size() ? model.animClipTokens[i] : 0;
        auto it = names.find(tok);
        if (it == names.end() || !taken.insert(tok).second) continue;
        model.animClips[i].name = it->second;
        kept.animClips.push_back(std::move(model.animClips[i]));
        kept.animClipTokens.push_back(tok);
        if (i < model.animClipBank.size()) kept.animClipBank.push_back(model.animClipBank[i]);
        if (i < model.animClipMoveSpeed.size()) kept.animClipMoveSpeed.push_back(model.animClipMoveSpeed[i]);
        if (i < model.animClipVis.size()) kept.animClipVis.push_back(std::move(model.animClipVis[i]));
        if (i < model.animClipConsts.size()) kept.animClipConsts.push_back(std::move(model.animClipConsts[i]));
        if (i < model.animClipUvs.size()) kept.animClipUvs.push_back(std::move(model.animClipUvs[i]));
    }
    model.animClips = std::move(kept.animClips);
    model.animClipTokens = std::move(kept.animClipTokens);
    model.animClipBank = std::move(kept.animClipBank);
    model.animClipMoveSpeed = std::move(kept.animClipMoveSpeed);
    model.animClipVis = std::move(kept.animClipVis);
    model.animClipConsts = std::move(kept.animClipConsts);
    model.animClipUvs = std::move(kept.animClipUvs);
}

nlohmann::json animator_json(const AnimatorPlan& plan, const std::vector<std::string>& clipNames,
                             const std::vector<float>& durations, const std::vector<float>& moveSpeeds,
                             bool metres) {
    using nlohmann::json;
    const double scale = metres ? 0.0254 : 1.0;
    auto clipName = [&](int c) { return c >= 0 && static_cast<size_t>(c) < clipNames.size() ? json(clipNames[c]) : json(nullptr); };
    json clips = json::array();
    for (int c : plan.clips) {
        const size_t i = static_cast<size_t>(c);
        clips.push_back({{"name", clipName(c)},
                         {"duration", i < durations.size() ? durations[i] : 0.0f},
                         {"moveSpeed", i < moveSpeeds.size() ? moveSpeeds[i] * scale : 0.0}});
    }
    json states = json::array();
    for (const AnimatorState& s : plan.states)
        states.push_back({{"name", s.name},
                          {"clip", clipName(s.clip)},
                          {"template", template_name(s.sequence)},
                          {"blendIn", s.blendIn},
                          {"flags", s.flags},
                          {"yawDegrees", s.yawDegrees}});
    json transitions = json::array();
    for (const AnimatorTransition& t : plan.transitions)
        transitions.push_back({{"from", t.from},
                               {"to", t.to},
                               {"event", t.event},
                               {"clip", clipName(t.clip)},
                               {"template", t.sequence ? json(template_name(t.sequence)) : json(nullptr)},
                               {"blendIn", t.blendIn},
                               {"blendOut", t.blendOut},
                               {"flags", t.flags}});
    json extras = json::object();
    for (const auto& [role, c] : plan.extras) extras[role] = clipName(c);
    return {{"format", "castlemist-animator"},
            {"version", 1},
            {"extras", extras},
            {"stance", std::string(1, plan.stance)},
            {"units", metres ? "metres" : "gw2"},
            {"clips", clips},
            {"states", states},
            {"transitions", transitions},
            {"missing", plan.missing}};
}

CharacterAnimator plan_character_animator(Gw2Dat& dat, const std::string& race_key, const std::string& stances, bool metres) {
    CharacterAnimator out;
    std::optional<composite::Composite> comp = load_composite(dat);
    const composite::CompositeRace* race = comp ? comp->race(race_key) : nullptr;
    if (!race) {
        out.error = "no Composite race " + race_key;
        return out;
    }
    out.skeletonFile = race->skeleton_file;

    // The one anic file: every machine and which model each drives.
    std::vector<uint32_t> hits = db::is_open() ? db::query_base_ids("", "anic", 0, false, false, 4) : std::vector<uint32_t>{};
    if (hits.empty() || hits[0] > dat.mft_data_list.size()) {
        out.error = "no anic (animation machine) file in the index";
        return out;
    }
    auto tpl = tpl::get_or_auto_load();
    if (!tpl) {
        out.error = "no packfile template";
        return out;
    }
    model::AnimMachineSet set;
    try {
        const MftData& e = dat.mft_data_list[hits[0] - 1];
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> bytes = e.compression_flag ? cmp::decompress_entry(raw) : std::move(raw);
        set = model::Extractor(bytes, *tpl).parseAnimMachines();
    } catch (const std::exception& ex) {
        out.error = std::string("anic: ") + ex.what();
        return out;
    }
    for (const model::AnimMachineModelRef& r : set.models)
        if (r.modelFileId == out.skeletonFile && r.machineIndex < set.machines.size()) out.machine = static_cast<int>(r.machineIndex);
    if (out.machine < 0) {
        out.error = "no machine drives skeleton file " + std::to_string(out.skeletonFile);
        return out;
    }

    std::optional<ModelPreview> rig = detail::load_model(dat, out.skeletonFile);
    if (!rig || rig->animClips.empty()) {
        out.error = "the race's clips did not load";
        return out;
    }
    const model::AnimMachine& machine = set.machines[static_cast<size_t>(out.machine)];
    std::vector<std::string> names(rig->animClips.size());
    std::vector<float> durations(rig->animClips.size());
    for (size_t i = 0; i < rig->animClips.size(); ++i) durations[i] = rig->animClips[i].duration;
    std::set<std::string> taken;
    auto exportName = [&](int c) {
        std::string& name = names[static_cast<size_t>(c)];
        if (!name.empty()) return;  // another stance already carries it
        const std::string base = clean_clip_name(rig->animClips[static_cast<size_t>(c)].name);
        name = base;
        for (int n = 2; !taken.insert(name).second; ++n) name = base + "_" + std::to_string(n);
        out.clipSelect.emplace_back(rig->animClipTokens[static_cast<size_t>(c)], name);
    };
    out.json = {{"format", "castlemist-animator"}, {"version", 2}, {"race", race_key}, {"machine", out.machine},
                {"skeletonFile", out.skeletonFile}, {"units", metres ? "metres" : "gw2"}, {"stances", nlohmann::json::array()}};
    for (char stance : stances) {
        if (stance < 'a' || stance > 'z') continue;
        AnimatorPlan plan = plan_animator(machine, rig->animClipTokens, stance, is_character_state);
        // Draw and stow belong to the stance, not the machine: only its own letter counts.
        for (const char* role : {"draw", "stow"}) {
            const uint64_t want = fill_stance(model::tokenizeBoneName(std::string("_gm") + role), stance);
            int c = -1;
            for (size_t i = 0; i < rig->animClipTokens.size() && c < 0; ++i)
                if (sequence_matches(want, rig->animClipTokens[i])) c = static_cast<int>(i);
            plan.extras.emplace_back(role, c);
            if (c >= 0 && !std::binary_search(plan.clips.begin(), plan.clips.end(), c))
                plan.clips.insert(std::upper_bound(plan.clips.begin(), plan.clips.end(), c), c);
        }
        for (int c : plan.clips) exportName(c);
        out.plans.push_back(std::move(plan));
    }
    if (out.plans.empty()) {
        out.error = "no stance letters in \"" + stances + "\"";
        return out;
    }
    for (const AnimatorPlan& plan : out.plans)
        out.json["stances"].push_back(animator_json(plan, names, durations, rig->animClipMoveSpeed, metres));

    // Emotes: the emoc file's emotes timed for this race's skeleton file, each by its clip
    // (the exact token, else the same name in another variant).
    out.json["emotes"] = nlohmann::json::array();
    std::vector<uint32_t> emoc = db::query_base_ids("", "emoc", 0, false, false, 4);
    if (!emoc.empty() && emoc[0] <= dat.mft_data_list.size()) {
        std::vector<model::EmoteAnimation> emotes;
        try {
            const MftData& e = dat.mft_data_list[emoc[0] - 1];
            std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
            std::vector<uint8_t> bytes = e.compression_flag ? cmp::decompress_entry(raw) : std::move(raw);
            emotes = model::Extractor(bytes, *tpl).parseEmotes();
        } catch (const std::exception&) {
        }
        std::set<std::string> labels;
        for (const model::EmoteAnimation& em : emotes) {
            auto t = std::find_if(em.timings.begin(), em.timings.end(),
                                  [&](const model::EmoteAnimation::Timing& x) { return x.modelFileId == out.skeletonFile; });
            if (t == em.timings.end()) continue;
            // An emote is up to three clips, told apart by the token's variant nibble: 4 the
            // intro, 8 the loop, 12 the outro (each part's length is its clip's length).
            auto part = [&](uint64_t variant) {
                const uint64_t want = (em.token & ~(0xFull << 60)) | (variant << 60);
                for (size_t i = 0; i < rig->animClipTokens.size(); ++i)
                    if (rig->animClipTokens[i] == want) return static_cast<int>(i);
                return -1;
            };
            int intro = t->intro > 0 ? part(4) : -1, loop = t->loop > 0 ? part(8) : -1, outro = t->outro > 0 ? part(12) : -1;
            if (intro < 0 && loop < 0) {  // a one-shot under another variant: the clip the table names
                for (size_t i = 0; i < rig->animClipTokens.size() && intro < 0; ++i)
                    if (rig->animClipTokens[i] == em.token) intro = static_cast<int>(i);
            }
            const int named = intro >= 0 ? intro : loop;
            if (named < 0) continue;
            const std::string label = emote_label(rig->animClips[static_cast<size_t>(named)].name);
            if (label.empty() || !labels.insert(label).second) continue;  // not a player emote, or a second variant
            auto clipOf = [&](int c) {
                if (c < 0) return nlohmann::json(nullptr);
                exportName(c);
                return nlohmann::json(names[static_cast<size_t>(c)]);
            };
            out.json["emotes"].push_back({{"label", label},
                                          {"intro", clipOf(intro)},
                                          {"loop", clipOf(loop)},
                                          {"outro", clipOf(outro)},
                                          {"blendIn", t->blendIn},
                                          {"blendOut", t->blendOut}});
        }
    }
    out.ok = true;
    return out;
}

}  // namespace castlemist::ripper
