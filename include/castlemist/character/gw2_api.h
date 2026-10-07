#ifndef CASTLEMIST_CHARACTER_GW2_API_H
#define CASTLEMIST_CHARACTER_GW2_API_H

// Typed calls to the official GW2 API (api.guildwars2.com/v2) for one account
// key: who the key is, its characters, what a character wears, and the public
// item / skin / dye data needed to resolve that gear. The key is sent only as
// an "Authorization: Bearer" header -- never in a URL or an error message.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "castlemist/character/http.h"
#include "castlemist/character/manifest.h"

namespace castlemist::character {

constexpr const char* kApiBase = "https://api.guildwars2.com";

/// Any failed call. `status()` is the HTTP status, 0 when the request never
/// completed (network) or the failure isn't an HTTP one.
class ApiError : public std::runtime_error {
public:
    ApiError(int status, const std::string& msg) : std::runtime_error(msg), status_(status) {}
    int status() const { return status_; }

private:
    int status_;
};

struct TokenInfo {
    std::string name;
    std::vector<std::string> permissions;
};

/// Of the scopes a character fetch needs (account, characters, builds, in that
/// order), the ones `t` lacks.
std::vector<std::string> missing_scopes(const TokenInfo& t);

struct CharacterCore {
    std::string name, race, gender, profession;
    int level = 0;
};

struct EquipmentEntry {
    uint32_t item_id = 0;
    std::string slot;                         // "Helm", "Coat", "WeaponA1", ...
    std::optional<uint32_t> skin;             // transmuted appearance, if any
    std::array<std::optional<uint32_t>, 4> dyes{};
    std::vector<uint32_t> upgrades, infusions;
};

struct EquipmentTab {
    int tab = 0;
    std::string name;
    bool is_active = false;
    std::vector<EquipmentEntry> equipment;
};

struct ApiItem {
    uint32_t id = 0;
    std::string name, type;
    std::optional<uint32_t> default_skin;
};

struct DyeSlot {
    uint32_t color_id = 0;
    std::string material;  // "cloth", "leather", "metal", "fur"
};
using DyeSlots = std::vector<std::optional<DyeSlot>>;

struct ApiSkin {
    uint32_t id = 0;
    std::string name, type, weight_class;
    DyeSlots dye_default;
    std::map<std::string, DyeSlots> dye_overrides;  // keyed as the API gives them: "CharrFemale", ...
};

struct ApiColor {
    uint32_t id = 0;
    std::string name;
    std::map<std::string, std::array<uint8_t, 3>> rgb;  // per material
    std::map<std::string, DyeShift> shift;              // per material
};

class Gw2Api {
public:
    Gw2Api(HttpClient& http, std::string key) : http_(http), key_(std::move(key)) {}

    TokenInfo token_info();
    std::vector<std::string> character_names();
    CharacterCore character_core(const std::string& name);
    std::vector<EquipmentTab> equipment_tabs(const std::string& name);
    /// Batched `?ids=` lookups (200 ids per request). Unknown ids are simply
    /// absent from the result (the API answers 206 Partial Content).
    std::map<uint32_t, ApiItem> items(const std::vector<uint32_t>& ids);
    std::map<uint32_t, ApiSkin> skins(const std::vector<uint32_t>& ids);
    std::map<uint32_t, ApiColor> colors(const std::vector<uint32_t>& ids);
    /// id -> display name from any public `?ids=` endpoint ("items", "maps",
    /// "mounts/skins", ...), batched the same way. These need no key: a Gw2Api
    /// made with an empty key sends no Authorization header at all.
    std::map<uint32_t, std::string> names(const std::string& endpoint, const std::vector<uint32_t>& ids);

private:
    std::string get_json(const std::string& path_and_query);

    HttpClient& http_;
    std::string key_;
};

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_GW2_API_H
