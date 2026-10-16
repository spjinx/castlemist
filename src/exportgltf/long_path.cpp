/// @file
/// @brief walk_files and remove_tree past MAX_PATH (long_path.h, review I4).
#include "long_path.h"

#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace castlemist::exportgltf {

namespace fs = std::filesystem;

#ifdef _WIN32

namespace {

std::error_code last_error() { return std::error_code(static_cast<int>(GetLastError()), std::system_category()); }

bool is_dot(const wchar_t* n) { return n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0)); }

/// Calls fn(child, data) for every entry of directory @p d (extended-length).
template <class F>
bool list_dir(const fs::path& d, std::error_code& ec, F&& fn) {
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = d.native() + L"\\*";
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return true;   // empty
        ec = std::error_code(static_cast<int>(e), std::system_category());
        return false;
    }
    bool ok = true;
    do {
        if (is_dot(fd.cFileName)) continue;
        if (!fn(d / fd.cFileName, fd)) { ok = false; break; }
    } while (FindNextFileW(h, &fd));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) { ec = last_error(); ok = false; }
    FindClose(h);
    return ok;
}

bool remove_dir_tree(const fs::path& d, std::error_code& ec) {
    std::vector<std::pair<fs::path, DWORD>> kids;
    if (!list_dir(d, ec, [&](const fs::path& c, const WIN32_FIND_DATAW& fd) {
            kids.emplace_back(c, fd.dwFileAttributes);
            return true;
        }))
        return false;
    for (const auto& [c, attr] : kids) {
        if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(c.c_str(), attr & ~DWORD(FILE_ATTRIBUTE_READONLY));
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {   // a link: remove it, never what it points at
            if (!((attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(c.c_str()) : DeleteFileW(c.c_str()))) {
                ec = last_error();
                return false;
            }
        } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (!remove_dir_tree(c, ec)) return false;
        } else if (!DeleteFileW(c.c_str())) {
            ec = last_error();
            return false;
        }
    }
    if (!RemoveDirectoryW(d.c_str())) { ec = last_error(); return false; }
    return true;
}

} // namespace

bool walk_files(const fs::path& dir, std::error_code& ec, const std::function<void(const fs::path&, uint64_t)>& fn) {
    ec.clear();
    std::vector<fs::path> todo{long_path(dir, true)};
    while (!todo.empty()) {
        const fs::path d = todo.back();
        todo.pop_back();
        if (!list_dir(d, ec, [&](const fs::path& c, const WIN32_FIND_DATAW& fd) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return true;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) todo.push_back(c);
                else fn(c, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
                return true;
            }))
            return false;
    }
    return true;
}

bool remove_tree(const fs::path& p, std::error_code& ec) {
    ec.clear();
    const fs::path lp = long_path(p, true);
    const DWORD attr = GetFileAttributesW(lp.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        ec = std::error_code(static_cast<int>(e), std::system_category());
        return false;
    }
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(lp.c_str(), attr & ~DWORD(FILE_ATTRIBUTE_READONLY));
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) return remove_dir_tree(lp, ec);
    if (!((attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(lp.c_str()) : DeleteFileW(lp.c_str()))) {
        ec = last_error();
        return false;
    }
    return true;
}

#else

bool walk_files(const fs::path& dir, std::error_code& ec, const std::function<void(const fs::path&, uint64_t)>& fn) {
    ec.clear();
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) fn(it->path(), it->file_size(e2));
    }
    return !ec;
}

bool remove_tree(const fs::path& p, std::error_code& ec) {
    ec.clear();
    fs::remove_all(p, ec);
    return !ec;
}

#endif

} // namespace castlemist::exportgltf
