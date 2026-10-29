/// @file
/// @brief The Particle Browser's testable model: model rows and their cache file, filters, and
///        the emitter describer (docs/superpowers/specs/2026-10-29-particle-browser-design.md).

#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <vector>

#include "castlemist/extract/model_types.h"
#include "castlemist/skillfx/effects.h"

namespace castlemist::ui::particles {

/// @brief One MODL that carries effects, as the Models tab lists it.
struct ModelRow {
    uint32_t fileId = 0;
    uint16_t clouds = 0, emitters = 0, streaks = 0, lightning = 0, lights = 0, meshEmitters = 0, bursts = 0,
             clipTracks = 0;
    uint8_t alignMask = 0;   ///< bit k: some emitter takes sprite case k (fx::plane_alignment_case)
};

/// @brief Writes the model index (dumps/index/gw2_fx_index.bin): the rows found so far and how
///        far through the MODL list the scan got, tied to the dat's fingerprint.
bool save_index(const std::filesystem::path& path, const std::string& fingerprint, uint32_t scannedUpTo,
                const std::vector<ModelRow>& rows);
/// @brief Reads it back; false (outputs untouched) for another dat, another version or a
///        truncated file.
bool load_index(const std::filesystem::path& path, const std::string& fingerprint, uint32_t& scannedUpTo,
                std::vector<ModelRow>& rows);

/// @brief Where the scan reads entries: entry i of @p count, a row when it carries effects.
///        The window's source reads MODL entries from the dat (Extractor::extractEffects).
struct ScanSource {
    size_t count = 0;
    std::function<std::optional<ModelRow>(size_t)> read;
};

/// @brief The Models tab's background scan: walks the source from @p startAt on a worker,
///        appending rows; saves the index every 2,000 entries, when it finishes and on stop().
class ModelScan {
public:
    ~ModelScan();
    void start(ScanSource src, std::filesystem::path cache, std::string fingerprint, uint32_t startAt,
               std::vector<ModelRow> rows);
    void stop();                       ///< asks the worker to stop, waits, saves
    void wait();                       ///< waits for the worker to finish on its own
    bool running() const { return running_; }
    size_t done() const { return done_; }    ///< entries walked so far (including startAt)
    size_t total() const { return total_; }
    std::vector<ModelRow> snapshot() const;  ///< the rows so far (copied under the lock)

private:
    void run(ScanSource src);
    void save();
    std::filesystem::path cache_;
    std::string fingerprint_;
    std::vector<ModelRow> rows_;
    mutable std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false};
    std::atomic<size_t> done_{0}, total_{0};
};

/// @brief The Models tab's filter bar. Feature boxes require the feature; alignCase -1 = any.
struct ModelFilter {
    bool streak = false, lightning = false, mesh = false, burst = false, tracks = false;
    int alignCase = -1;
    std::string search;   ///< an id, a range or a list (gallery::parse_id_query); other text matches nothing
};
/// @brief Indices of the rows that pass, in row order.
std::vector<size_t> filter_rows(const std::vector<ModelRow>& rows, const ModelFilter& f);

/// @brief The Effects tab's filter: @p cls is a skillfx::EffectClass value (-1 any); @p search
///        is an id query matched on the uid or the model fileId, else a case-insensitive
///        codename substring.
std::vector<size_t> filter_effects(const std::vector<castlemist::skillfx::EffectRow>& rows, int cls,
                                   const std::string& search);

/// @brief One line of the inspector: a decoded field, its value, what it means.
struct FieldLine {
    std::string group, label, value, meaning;
};
/// @brief Every field of an emitter, grouped (spawn, motion, colour, size, sprite, flipbook,
///        wind, flags), with the meanings the research note proves.
std::vector<FieldLine> describe_emitter(const ParticleEmitterCPU& e);

}  // namespace castlemist::ui::particles
