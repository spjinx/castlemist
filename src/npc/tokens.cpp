#include "castlemist/npc/tokens.h"

#include <cctype>

namespace castlemist::npc {

std::string decode_token(uint64_t token) {
    token &= 0x0FFFFFFFFFFFFFFFULL;
    std::string out;
    while (token != 0) {
        const unsigned g = static_cast<unsigned>(token & 0x1F);
        if (g > 26) return {};
        out.push_back(g == 0 ? ' ' : static_cast<char>('a' + g - 1));
        token >>= 5;
    }
    return out;
}

namespace {

struct Split {
    std::string prefix;
    std::string group;
};

Split split_clip(const std::string& name) {
    size_t end = name.size();
    bool had_digits = false;
    while (end > 0 && (std::isdigit(static_cast<unsigned char>(name[end - 1])) || name[end - 1] == ' ')) {
        if (name[end - 1] != ' ') had_digits = true;
        --end;
    }
    std::string s = name.substr(0, end);
    std::string prefix;
    if (had_digits && s.size() > 3 && std::isalpha(static_cast<unsigned char>(s[0])) &&
        std::isalpha(static_cast<unsigned char>(s[1])) && std::isalpha(static_cast<unsigned char>(s[2]))) {
        prefix = s.substr(0, 3);
        s.erase(0, 3);
    }
    size_t b = s.find_first_not_of(' ');
    size_t e = s.find_last_not_of(' ');
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    return {prefix, s};
}

} // namespace

std::string clip_group(const std::string& name) { return split_clip(name).group; }

std::string clip_prefix(const std::string& name) { return split_clip(name).prefix; }

} // namespace castlemist::npc
