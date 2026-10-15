/// @file
/// @brief make_dat_model_source: the world export's models and capped
///        textures from one shared, read-only parsed dat.
#include "castlemist/exportgltf/world_dat_source.h"

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/world/dat_read.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

namespace castlemist::exportgltf {

namespace {

/// Sub-project 2's R13: the workers share `dat`. load_dat_file closed the
/// dat's own stream; read_entry_bytes opens a file handle per read, and the
/// lookups load_model_by_fileid / decode_texture_capped make only read the
/// MFT tables (as src/ui/map_window_loader.cpp's model pool relies on).
class DatModelSource final : public ModelSource {
public:
    DatModelSource(Gw2Dat& dat, bool wantGame) : dat_(dat), wantGame_(wantGame) {}

    std::shared_ptr<ModelPreview> load(uint32_t /*index*/, uint32_t fileId) override {
        try {
            return load_model_by_fileid(dat_, fileId, ModelLoadOptions{wantGame_, /*decodeTextures=*/false});
        } catch (const castlemist::world::DatIoError&) {
            throw;
        } catch (...) {
            return nullptr;   // bad model data: a failed model, never a failed export
        }
    }

    std::shared_ptr<const CappedTexture> texture(uint32_t fileId, int maxSize) override {
        const std::pair<uint32_t, int> key{fileId, maxSize};
        std::shared_ptr<Slot> slot;
        bool mine = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto& s = cache_[key];
            if (!s) { s = std::make_shared<Slot>(); mine = true; }
            slot = s;
        }
        if (mine) {
            auto t = std::make_shared<CappedTexture>();
            try {
                decode_texture_capped(dat_, fileId, maxSize, *t);   // never throws (entry_extractor.h)
            } catch (...) {
                t->mips.clear();
            }
            std::shared_ptr<const CappedTexture> done = t->mips.empty() ? nullptr : std::move(t);
            {
                std::lock_guard<std::mutex> lk(mu_);
                slot->tex = done;
                slot->ready = true;
            }
            cv_.notify_all();
            return done;
        }
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return slot->ready; });
        return slot->tex;
    }

private:
    struct Slot { bool ready = false; std::shared_ptr<const CappedTexture> tex; };
    Gw2Dat& dat_;
    const bool wantGame_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::pair<uint32_t, int>, std::shared_ptr<Slot>> cache_;
};

} // namespace

std::unique_ptr<ModelSource> make_dat_model_source(Gw2Dat& dat, bool wantGame) {
    return std::make_unique<DatModelSource>(dat, wantGame);
}

} // namespace castlemist::exportgltf
