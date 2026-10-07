/// @file
/// @brief The info panel's "Game content" section: which game objects (items,
///        skins, maps, ...) use the selected file, with their names and chat
///        links. The formatting is kept free of Win32 and the network so it is
///        testable; content_names.cpp gathers the data and fetches the names.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "castlemist/format/content_map.h"

namespace castlemist::ui {

/// One content object that uses the file, and the items that unlock it.
struct LinkedObject {
    castlemist::cmap::ContentRef ref;
    std::vector<castlemist::cmap::ContentRef> granted_by;
};

/// A display name for (type, id): nullptr = not fetched yet, "" = the API has none.
using NameLookup = std::function<const std::string*(uint32_t type, uint32_t id)>;

inline uint64_t content_name_key(uint32_t type, uint32_t id) { return (static_cast<uint64_t>(type) << 32) | id; }

/// "Skin 6506  Astralaria  [&CmoZAAA=]": kind and id, the name when known (or
/// that it is being looked up), and the chat link when the type has one.
std::wstring format_content_ref(const castlemist::cmap::ContentRef& r, const NameLookup& name);

/// The whole section for `users` (the first of `total` objects that use the file).
std::wstring format_content_links(const std::vector<LinkedObject>& users, size_t total, const NameLookup& name);

} // namespace castlemist::ui
