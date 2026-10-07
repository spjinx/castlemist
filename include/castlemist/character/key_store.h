#ifndef CASTLEMIST_CHARACTER_KEY_STORE_H
#define CASTLEMIST_CHARACTER_KEY_STORE_H

// Named GW2 API keys, kept in a plain-text JSON file beside the castlemist
// checkout (git-ignored): {"keys":[{"name":"Main","key":"..."}]}. Plain text is
// a deliberate choice -- the file never leaves this machine, and the key only
// ever travels to api.guildwars2.com in an Authorization header.

#include <filesystem>
#include <string>
#include <vector>

namespace castlemist::character {

struct ApiKey {
    std::string name;
    std::string key;
};

class KeyStore {
public:
    /// Missing file -> true with an empty store. Malformed file -> false with
    /// `*error` set and the store left exactly as it was.
    bool load(const std::filesystem::path& file, std::string* error);
    /// Pretty-printed (2-space indent).
    bool save(const std::filesystem::path& file, std::string* error) const;

    const std::vector<ApiKey>& list() const { return keys_; }
    /// An existing name has its key replaced in place.
    void add(const std::string& name, const std::string& key);
    /// False if `from` is missing or `to` already exists.
    bool rename(const std::string& from, const std::string& to);
    bool remove(const std::string& name);
    const ApiKey* get(const std::string& name) const;

private:
    std::vector<ApiKey> keys_;
};

/// The first of `start_dir` and its ancestors holding a CMakeLists.txt that
/// declares `project(castlemist` (the checkout root for a dev build); otherwise
/// `start_dir` itself (a packaged release).
std::filesystem::path find_castlemist_root(const std::filesystem::path& start_dir);

/// find_castlemist_root(<this exe's directory>) / "api_keys.json".
std::filesystem::path default_key_file();

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_KEY_STORE_H
