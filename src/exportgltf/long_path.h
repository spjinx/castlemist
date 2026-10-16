/// @file
/// @brief Paths past MAX_PATH for the export writers (world export review I4).
///
/// Win32 file APIs stop at 260 characters unless the process is long-path aware
/// *and* the LongPathsEnabled registry switch is on. An extended-length path
/// (`\\?\C:\...`, backslashes only, absolute) works without either, so every
/// writer that may land deep (a world export under a Unity project's Assets/)
/// opens its files through long_path(). Measured with this toolchain (MinGW
/// libstdc++, 2026-10-15): ofstream, ifstream, rename, status, exists and
/// create_directory accept the prefixed form; create_directories and remove_all
/// do not (they walk the prefix as path elements and fail or loop), and
/// symlink_status / remove fail past 260 characters even when prefixed. So
/// make_dirs() creates the missing levels one at a time, and walk_files() /
/// remove_tree() go through Win32 directly (long_path.cpp).
/// @ingroup exportgltf
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace castlemist::exportgltf {

/// Below this many characters a path is used as it is (MAX_PATH 260 less the
/// 12 a directory must leave for an 8.3 file name).
inline constexpr size_t kLongPathAt = 248;

/// @p p unchanged when short; else `\\?\` + its absolute form with backslashes
/// (a UNC path becomes `\\?\UNC\server\share\...`). Not for display: callers
/// keep reporting the plain path.
/// @p always: prefix a short path too (for a walk whose children may be long).
inline std::filesystem::path long_path(const std::filesystem::path& p, bool always = false) {
#ifdef _WIN32
    if (p.native().rfind(L"\\\\?\\", 0) == 0) return p;
    if (!always && p.is_absolute() && p.native().size() < kLongPathAt) return p;
    std::error_code ec;
    std::filesystem::path a = std::filesystem::absolute(p, ec);
    if (ec || (!always && a.native().size() < kLongPathAt)) return p;
    a.make_preferred();
    std::wstring w = a.lexically_normal().native();
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();   // no trailing separator after a name
    if (!always && w.size() < kLongPathAt) return std::filesystem::path(w);
    if (w.rfind(L"\\\\", 0) == 0) return std::filesystem::path(L"\\\\?\\UNC\\" + w.substr(2));
    return std::filesystem::path(L"\\\\?\\" + w);
#else
    (void)always;
    return p;
#endif
}

/// create_directories that also works past MAX_PATH: the levels that exist or
/// are short are made as usual, the rest one at a time through long_path().
inline bool make_dirs(const std::filesystem::path& p, std::error_code& ec) {
    namespace fs = std::filesystem;
    ec.clear();
    fs::path a = fs::absolute(p, ec);
    if (ec) return false;
    if (a.native().size() < kLongPathAt) {
        fs::create_directories(p, ec);
        return !ec;
    }
    a = a.lexically_normal();
    fs::path cur;
    std::vector<fs::path> tail;
    for (const fs::path& part : a) {
        if (part.empty()) continue;
        if (tail.empty() && (cur / part).native().size() < kLongPathAt) cur /= part;
        else tail.push_back(part);
    }
    if (!cur.empty()) {
        fs::create_directories(cur, ec);   // false with no error when it exists
        if (ec) return false;
    }
    for (const fs::path& part : tail) {
        cur /= part;
        const fs::path lp = long_path(cur);
        std::error_code e2;
        if (fs::is_directory(lp, e2)) continue;
        fs::create_directory(lp, ec);
        if (ec) return false;
    }
    return true;
}

/// Every regular file under @p dir (any depth): fn(path, bytes), the path in
/// extended-length form. Reparse points (junctions, symlinks) are not followed.
/// False (with @p ec) when a directory could not be listed. long_path.cpp: Win32
/// FindFirstFileW, because libstdc++'s status / symlink_status / remove fail on
/// some prefixed paths past MAX_PATH (measured: "No such file or directory").
bool walk_files(const std::filesystem::path& dir, std::error_code& ec,
                const std::function<void(const std::filesystem::path&, uint64_t)>& fn);

/// remove_all that also works past MAX_PATH: children first, each through its
/// extended-length path; a reparse point is removed itself, never followed.
/// True when nothing is left (or nothing was there).
bool remove_tree(const std::filesystem::path& p, std::error_code& ec);

} // namespace castlemist::exportgltf
