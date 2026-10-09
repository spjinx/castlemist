#include "castlemist/db/data_status.h"

#include "castlemist/native/gw2dat.h"

#include "sqlite3.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <set>
#include <tuple>

namespace castlemist::db {

using nlohmann::json;
namespace fs = std::filesystem;

// ---- fingerprints

std::string DatFingerprint::to_string() const {
    return std::to_string(file_size) + ":" + std::to_string(mft_offset) + ":" + std::to_string(mft_size);
}

std::optional<DatFingerprint> DatFingerprint::parse(const std::string& s) {
    unsigned long long size = 0, off = 0;
    unsigned long mft = 0;
    int used = 0;
    if (std::sscanf(s.c_str(), "%llu:%llu:%lu%n", &size, &off, &mft, &used) != 3 ||
        static_cast<size_t>(used) != s.size())
        return std::nullopt;
    DatFingerprint fp{size, off, static_cast<uint32_t>(mft)};
    if (!fp.valid()) return std::nullopt;
    return fp;
}

DatFingerprint fingerprint_of(const Gw2Dat& dat) {
    return {dat.file_info.file_size, dat.dat_header.mft_offset, dat.dat_header.mft_size};
}

std::optional<DatFingerprint> read_dat_fingerprint(const fs::path& dat_path) {
    std::error_code ec;
    const uint64_t size = fs::file_size(dat_path, ec);
    if (ec) return std::nullopt;
    std::ifstream f(dat_path, std::ios::binary);
    DatHeader h{};
    if (!f.read(reinterpret_cast<char*>(&h), sizeof h)) return std::nullopt;
    if (h.identifier[0] != 'A' || h.identifier[1] != 'N') return std::nullopt;
    DatFingerprint fp{size, h.mft_offset, h.mft_size};
    if (!fp.valid()) return std::nullopt;
    return fp;
}

std::optional<uint32_t> pe_timestamp(const fs::path& exe_path) {
    std::ifstream f(exe_path, std::ios::binary);
    uint8_t mz[0x40];
    if (!f.read(reinterpret_cast<char*>(mz), sizeof mz) || mz[0] != 'M' || mz[1] != 'Z') return std::nullopt;
    const uint32_t pe = mz[0x3C] | (mz[0x3D] << 8) | (mz[0x3E] << 16) | (static_cast<uint32_t>(mz[0x3F]) << 24);
    uint8_t coff[12];
    f.seekg(pe);
    if (!f.read(reinterpret_cast<char*>(coff), sizeof coff) || coff[0] != 'P' || coff[1] != 'E' || coff[2] || coff[3])
        return std::nullopt;
    // Signature (4), Machine (2), NumberOfSections (2), then TimeDateStamp.
    return coff[8] | (coff[9] << 8) | (coff[10] << 16) | (static_cast<uint32_t>(coff[11]) << 24);
}

const char* to_string(Freshness f) {
    switch (f) {
    case Freshness::Ok: return "Current";
    case Freshness::Optional: return "Not set up";
    case Freshness::Unknown: return "Unknown";
    case Freshness::Stale: return "Stale";
    case Freshness::Missing: return "Missing";
    }
    return "?";
}

// ---- data_stamps.json

void StampStore::load(const fs::path& file) {
    *this = StampStore{};
    std::ifstream f(file, std::ios::binary);
    if (!f) return;
    try {
        json j = json::parse(f);
        if (auto d = j.find("dat"); d != j.end() && d->is_object()) {
            dat_ = DatFingerprint::parse(d->value("fingerprint", std::string()));
            dat_first_seen_ = d->value("first_seen", int64_t{0});
        }
        if (auto fs_ = j.find("files"); fs_ != j.end() && fs_->is_object()) {
            for (const auto& [key, v] : fs_->items()) {
                if (!v.is_object()) continue;
                auto fp = DatFingerprint::parse(v.value("dat", std::string()));
                if (fp) files_[key] = Stamp{*fp, v.value("at", int64_t{0})};
            }
        }
    } catch (const std::exception&) {
        *this = StampStore{};  // a corrupt cache only costs a rebuild prompt
    }
}

bool StampStore::save(const fs::path& file) const {
    json j = json::object();
    if (dat_) j["dat"] = {{"fingerprint", dat_->to_string()}, {"first_seen", dat_first_seen_}};
    json files = json::object();
    for (const auto& [key, s] : files_) files[key] = {{"dat", s.dat.to_string()}, {"at", s.at}};
    j["files"] = std::move(files);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << '\n';
    return static_cast<bool>(f);
}

std::optional<Stamp> StampStore::get(const std::string& key) const {
    auto it = files_.find(key);
    if (it == files_.end()) return std::nullopt;
    return it->second;
}

void StampStore::set(const std::string& key, const Stamp& s) { files_[key] = s; }
void StampStore::erase(const std::string& key) { files_.erase(key); }

bool StampStore::observe_dat(const DatFingerprint& now_fp, int64_t now) {
    if (!now_fp.valid()) return false;
    if (!dat_) {
        dat_ = now_fp;
        dat_first_seen_ = 0;
        return false;
    }
    if (*dat_ == now_fp) return false;
    dat_ = now_fp;
    dat_first_seen_ = now;
    return true;
}

// ---- verdicts

Verdict judge_stamped(const std::optional<Stamp>& stamp, const DatFingerprint& now) {
    if (!stamp) return {Freshness::Unknown, "Built before castlemist recorded which game build a file came from."};
    if (!now.valid()) return {Freshness::Unknown, "Open Gw2.dat to compare."};
    if (stamp->dat == now) return {Freshness::Ok, "Built from the current Gw2.dat."};
    return {Freshness::Stale, "Built from an older Gw2.dat: the game has patched since."};
}

Verdict judge_index(const std::map<std::string, std::string>& meta, const DatFingerprint& now, size_t now_entries,
                    const std::string& now_dat_path) {
    auto get = [&](const char* k) -> std::string {
        auto it = meta.find(k);
        return it == meta.end() ? std::string() : it->second;
    };
    if (!now.valid()) return {Freshness::Unknown, "Open Gw2.dat to compare."};
    const std::string built_path = get("dat_path");
    if (built_path.empty() && get("dat_size").empty())
        return {Freshness::Stale, "The index never finished building (it has no meta stamp)."};
    // Paths are compared loosely: the same archive reached through a different
    // case or slash direction is still the same archive.
    auto norm = [](std::string s) {
        for (char& c : s) {
            if (c == '/') c = '\\';
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    if (!now_dat_path.empty() && !built_path.empty() &&
        fs::path(norm(built_path)).filename() != fs::path(norm(now_dat_path)).filename())
        return {Freshness::Stale, "Built from a different archive (" + built_path + ")."};
    if (auto fp = DatFingerprint::parse(get("dat_fingerprint"))) {
        if (*fp == now) return {Freshness::Ok, "Built from the current Gw2.dat."};
        return {Freshness::Stale, "Built from an older Gw2.dat: the game has patched since. Rebuild (only changed "
                                  "entries are re-read)."};
    }
    const std::string size = get("dat_size"), entries = get("mft_entries");
    if (size == std::to_string(now.file_size) && entries == std::to_string(now_entries))
        return {Freshness::Ok, "Archive size and entry count match the current Gw2.dat."};
    return {Freshness::Stale, "Archive size or entry count differ from the current Gw2.dat: the game has patched "
                              "since. Rebuild (only changed entries are re-read)."};
}

Verdict judge_by_mtime(int64_t file_mtime, int64_t dat_first_seen) {
    if (dat_first_seen == 0)
        return {Freshness::Ok, "No game patch seen since castlemist started tracking the dat."};
    if (file_mtime >= dat_first_seen) return {Freshness::Ok, "Made after the last game patch castlemist saw."};
    return {Freshness::Stale, "Made before the last game patch castlemist saw."};
}

Verdict judge_template(const json* source, std::optional<uint32_t> exe_ts, int64_t json_mtime, int64_t exe_mtime,
                       int unmapped) {
    if (unmapped > 0)
        return {Freshness::Stale, std::to_string(unmapped) + " chunk version" + (unmapped == 1 ? "" : "s") +
                                      " in the index have no struct in it: the game patched after it was "
                                      "generated."};
    if (source && source->is_object() && exe_ts) {
        if (source->value("peTimestamp", uint64_t{0}) == *exe_ts)
            return {Freshness::Ok, "Generated from the installed Gw2-64.exe."};
        return {Freshness::Stale, "Generated from a different Gw2-64.exe build than the one installed."};
    }
    if (exe_mtime != 0 && json_mtime != 0 && exe_mtime > json_mtime) {
        // Without provenance this is only a hint: the launcher touches the exe on
        // patches that leave every chunk version alone.
        if (unmapped == 0)
            return {Freshness::Ok, "Gw2-64.exe is newer, but every chunk version in the index has a struct."};
        return {Freshness::Unknown, "Gw2-64.exe is newer than the template; open an index to check it against the "
                                    "game data."};
    }
    if (unmapped == 0) return {Freshness::Ok, "Every chunk version in the index has a struct."};
    return {Freshness::Unknown, "Records no source exe; open an index to check it against the game data."};
}

// ---- template vs index

std::string resolve_variant(const json& tpl, const std::string& container, const std::string& fourcc, int version) {
    const std::string vs = std::to_string(version);
    auto ft = tpl.find("fileTypes");
    if (ft != tpl.end()) {
        auto c = ft->find(container);
        if (c != ft->end()) {
            auto f = c->find(fourcc);
            if (f != c->end()) {
                auto v = f->find(vs);
                if (v != f->end()) return v->get<std::string>();
            }
        }
    }
    auto ch = tpl.find("chunks");
    if (ch != tpl.end()) {
        auto f = ch->find(fourcc);
        if (f != ch->end()) {
            auto v = f->find(vs);
            if (v != f->end()) return v->get<std::string>();
            if (!f->empty()) return "?v" + vs;  // fourcc known, version not mapped
        }
    }
    return "";  // unknown chunk
}

namespace {

sqlite3* open_readonly(const fs::path& db_path) {
    sqlite3* db = nullptr;
    const std::u8string u8 = db_path.u8string();
    const std::string utf8(u8.begin(), u8.end());
    if (sqlite3_open_v2(utf8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return nullptr;
    }
    return db;
}

std::string col(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

} // namespace

std::vector<ChunkVersion> index_chunk_versions(const fs::path& db_path) {
    std::vector<ChunkVersion> out;
    sqlite3* db = open_readonly(db_path);
    if (!db) return out;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT DISTINCT IFNULL(e.container,''), c.fourcc, c.version FROM chunks c JOIN entries e USING(base_id)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back({col(st, 0), col(st, 1), sqlite3_column_int(st, 2)});
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

std::map<std::string, std::string> index_meta(const fs::path& db_path) {
    std::map<std::string, std::string> out;
    sqlite3* db = open_readonly(db_path);
    if (!db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key, value FROM meta", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out[col(st, 0)] = col(st, 1);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

std::vector<ChunkVersion> unmapped_versions(const json& tpl, const std::vector<ChunkVersion>& seen) {
    std::vector<ChunkVersion> out;
    std::set<std::tuple<std::string, int>> reported;  // one line per fourcc+version, whatever the container
    for (const ChunkVersion& c : seen) {
        const std::string v = resolve_variant(tpl, c.container, c.fourcc, c.version);
        if (v.rfind("?v", 0) != 0) continue;
        if (reported.insert({c.fourcc, c.version}).second) out.push_back(c);
    }
    return out;
}

} // namespace castlemist::db
