/// @file
/// @brief A character's GW2 locomotion state machine, resolved to clips:
///        the plan a Unity Animator Controller is built from.
/// @ingroup ripper
///
/// GW2 drives a character with an animation machine (`anic` file, `mach`
/// chunk; docs/research/gw2-anim-blend-trees.md): named states that play a
/// clip, transitions that may play a transition clip on the way, crossfade
/// times on both, and SetRotation on diagonal states (the straight clip,
/// turned). Clip ids are name templates whose empty slots the client fills;
/// the first is the stance (`u` unarmed, weapon letters otherwise).
///
/// plan_animator() resolves one machine against one model's clips for one
/// stance and keeps the states a caller asks for. Nothing here knows about
/// Unity or VRChat; animator_json() writes the plan for the editor script.

#pragma once

#include "castlemist/extract/model_types.h"
#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace castlemist::ripper {

/// One machine state, resolved.
struct AnimatorState {
    std::string name;      ///< GW2's state name ("Ground Run Forward Left")
    int clip = -1;         ///< index into the clip tokens given to plan_animator; -1 = none found
    uint64_t sequence = 0; ///< the template it came from (0 = the state plays nothing)
    float blendIn = 0;     ///< PlayAnimation.blendInTime (s): crossfade into this state's clip
    uint32_t flags = 0;    ///< PlayAnimation.flags, as stored
    float yawDegrees = 0;  ///< SetRotation about GW2's up axis (+Z); 0 = none
};

/// One machine transition, resolved.
struct AnimatorTransition {
    std::string from, to;
    std::string event;   ///< the transition's name ("Land", "Land Hard"); "" = the client's choice
    int clip = -1;       ///< transition clip played on the way; -1 = none
    uint64_t sequence = 0;
    float blendIn = -1;  ///< from the transition's action; -1 = it has none (use the target's)
    float blendOut = -1; ///< -1 = the stored "default"
    uint32_t flags = 0;
};

struct AnimatorPlan {
    char stance = 'u';
    std::vector<AnimatorState> states;
    std::vector<AnimatorTransition> transitions;  ///< only between kept states
    std::vector<int> clips;                       ///< every clip index used, sorted, unique
    std::vector<std::string> missing;             ///< templates no clip matched (decoded)
    /// Clips outside the machine the stance also has, by role ("draw", "stow"); -1 = none.
    std::vector<std::pair<std::string, int>> extras;
};

/// The template with its empty first slot filled by `stance` (a-z).
/// Templates that already name a stance come back unchanged.
uint64_t fill_stance(uint64_t sequence, char stance);

/// True when `token` has every letter `sequence` names (empty slots match
/// anything; the variant nibble 60-63 is ignored).
bool sequence_matches(uint64_t sequence, uint64_t token);

/// The clip a template plays for `stance`: an exact stance match first, then
/// the unarmed one (`u`), then any. Within each: variant 0, then the part the
/// state wants (8, the loop, for a looping state; 4, the intro, otherwise),
/// then the rest. -1 when nothing matches.
int find_clip(uint64_t sequence, const std::vector<uint64_t>& tokens, char stance, bool loops = true);

/// Resolves `machine` against clips `tokens`, keeping the states `keep`
/// accepts (all when empty).
AnimatorPlan plan_animator(const model::AnimMachine& machine, const std::vector<uint64_t>& tokens, char stance,
                           const std::function<bool(const std::string&)>& keep = {});

/// The locomotion states: ground idle, alert (a weapon stance's ready idle),
/// fidget, walk, run, sprint, turn; air jump (and its prepare) and fall; water
/// idle, fidget, float, sink, swim and surface swim. No mounts, gliding,
/// limping, evades, hits or deaths.
bool is_locomotion_state(const std::string& name);

/// Every state a character on foot has: the locomotion above plus evades,
/// slides, gliding, hits, knockbacks and knockdowns with their recoveries,
/// stuns, downed, deaths, kneel, pickups, limping. Only mounts are left out.
bool is_character_state(const std::string& name);

/// A clip name safe for glTF/FBX/Unity ("ugmrun    f " -> "ugmrun_f"):
/// runs of spaces become one '_', ends trimmed.
std::string clean_clip_name(const std::string& granny_name);

/// Keeps only the clips listed (by token64), renamed. Every parallel clip
/// vector of `model` (bank, tokens, speeds, vis, consts, uvs) follows.
void select_clips(ModelPreview& model, const std::vector<std::pair<uint64_t, std::string>>& keep);

/// The plan for the editor script: states with their clip's export name,
/// duration and authored ground speed (metres per second when `metres`),
/// transitions, and the stance.
nlohmann::json animator_json(const AnimatorPlan& plan, const std::vector<std::string>& clipNames,
                             const std::vector<float>& durations, const std::vector<float>& moveSpeeds,
                             bool metres);

/// The player emote a clip is ("ugesit04" -> "Sit", "thumbsup" -> "Thumbs Up"),
/// or "" when the emote table's clip is not one (NPC gestures, skill clips,
/// gathering, mounted variants).
std::string emote_label(const std::string& clip_name);

/// A playable race's locomotion animator, from the dat.
struct CharacterAnimator {
    bool ok = false;
    std::string error;
    uint32_t skeletonFile = 0;  ///< the race's Composite skeleton file (its clips and machine binding)
    int machine = -1;           ///< index into the anic file's machines
    std::vector<AnimatorPlan> plans;  ///< one per stance asked for, in that order
    /// For AssemblyOptions::clip_select: every clip any plan uses, by token, under its export name.
    std::vector<std::pair<uint64_t, std::string>> clipSelect;
    /// Version 2: race / machine / skeleton / units, "stances" (one animator_json() each) and
    /// "emotes" (the race's emotes from the emoc file: clip, label, blend, intro/loop/outro).
    nlohmann::json json;
};

/// `race_key` is the Composite race name ("SylvariFemale"); `stances` the stance
/// letters to plan ("u" unarmed, "w" one-handed: axe, sword, mace..., "g"
/// greatsword, ...). Reads the anic file through the index DB (it must be open)
/// and the race's clips (streamed banks included, so this takes seconds).
/// `metres` scales ground speeds as the avatar is scaled.
CharacterAnimator plan_character_animator(Gw2Dat& dat, const std::string& race_key, const std::string& stances, bool metres);

}  // namespace castlemist::ripper
