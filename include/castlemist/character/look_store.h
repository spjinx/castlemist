#ifndef CASTLEMIST_CHARACTER_LOOK_STORE_H
#define CASTLEMIST_CHARACTER_LOOK_STORE_H

// Per-character look presets -- the face, hair style and colours the GW2 API
// does not expose -- kept in a plain-text JSON file beside the castlemist
// checkout (git-ignored), keyed by character name:
//   {"looks": {"Musa Blossom": {"face": 3, "hair": 12, "skin_color": 1048, ...}}}
// Colours are colour ids from the race's character-creator palettes
// (ripper/look.h), stable across game builds; 0 = as the texture is authored.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace castlemist::character {

struct CharacterLook {
    int face = 0;               // index into the race's faces
    int hair = 0;               // index into the race's hair styles
    uint32_t skin_color = 0;    // colour id in the race's skin palette
    uint32_t hair_color = 0;    // colour id in the race's hair palette
    uint32_t hair_color2 = 0;   // the hair's second dye channel; 0 = same as hair_color
    int ears = 0;               // index into the race's ears
    uint32_t eye_color = 0;     // colour id in the race's eye palette
    int pattern = -1;           // index into the race's skin patterns; -1 = none
    uint32_t pattern_color = 0; // colour id in the race's pattern palette
    uint32_t glow_color = 0;    // sylvari: colour id in the glow palette; 0 = no glow
    float glow_intensity = 1;   // 0..1

    bool operator==(const CharacterLook&) const = default;
};

class LookStore {
public:
    /// Missing file -> true with an empty store. Malformed file -> false with
    /// `*error` set and the store left exactly as it was.
    bool load(const std::filesystem::path& file, std::string* error);
    /// Pretty-printed (2-space indent).
    bool save(const std::filesystem::path& file, std::string* error) const;

    std::optional<CharacterLook> get(const std::string& character) const;
    void set(const std::string& character, const CharacterLook& look);
    bool remove(const std::string& character);
    const std::map<std::string, CharacterLook>& all() const { return looks_; }

private:
    std::map<std::string, CharacterLook> looks_;
};

/// find_castlemist_root(<this exe's directory>) / "character_looks.json".
std::filesystem::path default_look_file();

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_LOOK_STORE_H
