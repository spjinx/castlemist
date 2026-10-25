#include "castlemist/world/anim.h"

namespace castlemist::world {

int resolve_prop_clip(const std::vector<uint64_t>& clipTokens, uint64_t animSequence) {
    if (animSequence == 0) return -1;
    for (size_t i = 0; i < clipTokens.size(); ++i)
        if (clipTokens[i] == animSequence) return int(i);
    return -1;
}

std::vector<int> resolve_prop_sequence(const std::vector<uint64_t>& clipTokens, uint64_t animSequence) {
    const int exact = resolve_prop_clip(clipTokens, animSequence);
    if (exact >= 0) return {exact};
    if (animSequence == 0) return {};
    constexpr uint64_t kBase = 0x0FFFFFFFFFFFFFFFull;
    int intro = -1, loop = -1;
    for (size_t i = 0; i < clipTokens.size(); ++i) {
        if ((clipTokens[i] & kBase) != animSequence) continue;
        const uint64_t suffix = clipTokens[i] >> 60;
        if (suffix == 4 && intro < 0) intro = int(i);
        else if (suffix == 8 && loop < 0) loop = int(i);
    }
    std::vector<int> out;
    if (intro >= 0) out.push_back(intro);
    if (loop >= 0) out.push_back(loop);
    return out;
}

} // namespace castlemist::world
