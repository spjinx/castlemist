#include "castlemist/world/anim.h"

namespace castlemist::world {

int resolve_prop_clip(const std::vector<uint64_t>& clipTokens, uint64_t animSequence) {
    if (animSequence == 0) return -1;
    for (size_t i = 0; i < clipTokens.size(); ++i)
        if (clipTokens[i] == animSequence) return int(i);
    return -1;
}

} // namespace castlemist::world
