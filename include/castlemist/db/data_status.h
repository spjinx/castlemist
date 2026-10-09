/// @file
/// @brief Is each file an export depends on still current for the Gw2.dat it
///        will be used with?
///
/// castlemist derives several files from one game build: the index, the content
/// map, the game-name cache, the struct template, the string keys. None of them
/// carried a version, so after a game patch they kept loading and quietly gave
/// wrong answers. This module gives each one a verdict.
///
/// The yardstick is a DatFingerprint: the archive's size and where its MFT sits
/// and how big it is. Every game patch rewrites the MFT, so the fingerprint
/// changes with every patch, yet it costs one 40-byte header read to take.
/// Files castlemist builds itself are stamped with the fingerprint they were
/// built against (data_stamps.json, or the index's own meta table). Files made
/// by outside tools are judged by provenance (the struct template records the
/// executable it came from) or, failing that, by modification time against the
/// moment castlemist first saw the current fingerprint.
///
/// No Win32, no UI: the verdict functions are pure so the rules can be tested.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

struct Gw2Dat;

namespace castlemist::db {

/// @brief Cheap identity of one state of a Gw2.dat.
struct DatFingerprint {
    uint64_t file_size = 0;
    uint64_t mft_offset = 0;
    uint32_t mft_size = 0;

    bool valid() const { return file_size != 0 && mft_offset != 0; }
    /// "size:mftOffset:mftSize", decimal. What the stamps and the index store.
    std::string to_string() const;
    static std::optional<DatFingerprint> parse(const std::string& s);
    bool operator==(const DatFingerprint&) const = default;
};

/// From an archive already open in memory.
DatFingerprint fingerprint_of(const Gw2Dat& dat);

/// Reads only the archive header, so it is cheap enough to poll. Used to notice
/// the game patching the dat while castlemist still has the old MFT loaded.
std::optional<DatFingerprint> read_dat_fingerprint(const std::filesystem::path& dat_path);

/// The COFF TimeDateStamp of a PE file: different for every client build.
std::optional<uint32_t> pe_timestamp(const std::filesystem::path& exe_path);

/// @brief Where a file stands. Ordered from best to worst for the badge.
enum class Freshness {
    Ok,        ///< Built against the current game data.
    Optional,  ///< Not present, and nothing requires it.
    Unknown,   ///< Present, but nothing records what it was built against.
    Stale,     ///< Built against an older game build: rebuild it.
    Missing,   ///< Not present, and an export needs it.
};

const char* to_string(Freshness f);

struct Verdict {
    Freshness state = Freshness::Unknown;
    std::string detail;  ///< One sentence, shown beside the state.
};

/// A stamp: which dat a file was built from, and when (unix seconds).
struct Stamp {
    DatFingerprint dat;
    int64_t at = 0;
};

/// @brief data_stamps.json: one Stamp per derived file, plus the dat history.
///
/// {"dat":{"fingerprint":"...","first_seen":N},
///  "files":{"content_map":{"dat":"...","at":N}, ...}}
class StampStore {
public:
    /// Missing or unreadable file -> empty store (it is a cache, never fatal).
    void load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;

    std::optional<Stamp> get(const std::string& key) const;
    void set(const std::string& key, const Stamp& s);
    void erase(const std::string& key);

    /// Records `now_fp` as the current dat. Returns true when it differs from the
    /// one last recorded, and then remembers `now` as the moment it changed. The
    /// very first observation has no "before", so first_seen stays 0 ("unknown").
    bool observe_dat(const DatFingerprint& now_fp, int64_t now);
    std::optional<DatFingerprint> last_dat() const { return dat_; }
    /// When the current dat was first seen, 0 if it was the first ever observed.
    int64_t dat_first_seen() const { return dat_first_seen_; }

private:
    std::optional<DatFingerprint> dat_;
    int64_t dat_first_seen_ = 0;
    std::map<std::string, Stamp> files_;
};

// ---- verdicts (pure)

/// A file stamped by castlemist when it built it.
Verdict judge_stamped(const std::optional<Stamp>& stamp, const DatFingerprint& now);

/// The index: its meta table against the open dat. Indexes built before the
/// fingerprint was stamped fall back to dat_size + mft_entries.
Verdict judge_index(const std::map<std::string, std::string>& meta, const DatFingerprint& now,
                    size_t now_entries, const std::string& now_dat_path);

/// A file only an outside tool writes, judged by modification time against
/// when the current dat was first seen (0 = unknown).
Verdict judge_by_mtime(int64_t file_mtime, int64_t dat_first_seen);

/// The struct template. `source` is the template's "source" object if it has
/// one ({"exe":..., "peTimestamp":N, "size":N}); `exe_ts` is the client's
/// current PE timestamp. `unmapped` counts chunk versions in the index the
/// template has no struct for (-1 = not checked).
Verdict judge_template(const nlohmann::json* source, std::optional<uint32_t> exe_ts, int64_t json_mtime,
                       int64_t exe_mtime, int unmapped);

// ---- the struct-template check against the index

/// Distinct (container, fourcc, version) chunk triples the index has seen.
struct ChunkVersion {
    std::string container, fourcc;
    int version = 0;
};

/// Read through a private read-only connection, so it is safe on a worker
/// thread while the UI holds the shared one. Empty on any error.
std::vector<ChunkVersion> index_chunk_versions(const std::filesystem::path& db_path);

/// The index's meta table, through a private read-only connection.
std::map<std::string, std::string> index_meta(const std::filesystem::path& db_path);

/// Those of `seen` whose fourcc the template knows but whose version it lacks:
/// the signature of a game patch the template predates. Unknown fourccs are not
/// counted -- the template has never described every chunk.
std::vector<ChunkVersion> unmapped_versions(const nlohmann::json& tpl, const std::vector<ChunkVersion>& seen);

/// Same rule the index builder uses to name a chunk's struct.
std::string resolve_variant(const nlohmann::json& tpl, const std::string& container, const std::string& fourcc,
                            int version);

} // namespace castlemist::db
