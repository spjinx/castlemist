#include "castlemist/ripper/character_export.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "castlemist/character/manifest_json.h"
#include "castlemist/db/index_db.h"
#include "castlemist/native/cmp_decompress_method0.hpp"

namespace castlemist::ripper {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr uint32_t kCompositeFileId = 154681;  // fallback when no index DB is open

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string to_utf8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string sanitize(std::string s) {
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    return s.empty() ? "piece" : s;
}

} // namespace

size_t CharacterExportReport::exported() const {
    size_t n = 0;
    for (const auto& [slot, r] : pieces) n += r.ok;
    return n;
}

std::optional<composite::Composite> load_composite(Gw2Dat& dat) {
    uint32_t base = 0;
    if (db::is_open()) {
        std::vector<uint32_t> hits = db::query_base_ids("", "cmpc", 0, false, false, 4);
        if (!hits.empty()) base = hits[0];
    }
    if (base == 0) base = get_by_base_id(dat, kCompositeFileId);
    if (base == 0 || base > dat.mft_data_list.size()) return std::nullopt;
    try {
        const MftData& e = dat.mft_data_list[base - 1];
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> bytes = e.compression_flag ? cmp::decompress_entry(raw) : std::move(raw);
        return composite::parse_composite(bytes);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

CharacterExportReport export_character(const character::CharacterManifest& manifest, const std::string& dat_path,
                                       const std::string& out_dir) {
    CharacterExportReport rep;
    Gw2Dat dat;
    try {
        load_dat_file(dat, dat_path);
    } catch (const std::exception& e) {
        rep.error = std::string("cannot open the dat: ") + e.what();
        return rep;
    }
    std::optional<composite::Composite> comp = load_composite(dat);
    if (!comp) {
        rep.error = "Composite file not found in this dat (open the index DB, or rebuild it)";
        return rep;
    }
    std::error_code ec;
    fs::create_directories(from_utf8(out_dir), ec);
    if (ec) {
        rep.error = "cannot create " + out_dir + ": " + ec.message();
        return rep;
    }

    PieceContext ctx{&dat, &*comp, manifest.race + manifest.gender};
    json report = json::array();
    for (size_t i = 0; i < manifest.pieces.size(); ++i) {
        const character::ManifestPiece& p = manifest.pieces[i];
        char prefix[8];
        std::snprintf(prefix, sizeof prefix, "%02zu_", i + 1);
        fs::path file = from_utf8(out_dir) / from_utf8(prefix + sanitize(p.slot) + "_" + sanitize(p.skin_name) + ".glb");
        PieceExportResult r = export_piece(ctx, p, to_utf8(file));
        report.push_back({{"slot", p.slot}, {"status", r.status}, {"file", r.ok ? to_utf8(file.filename()) : std::string()},
                          {"mesh", r.mesh}, {"texture_base", r.texture_base}, {"dyed_channels", r.dyed_channels},
                          {"undyed_channels", r.undyed_channels}, {"reason", r.reason}});
        rep.pieces.emplace_back(p.slot, std::move(r));
    }

    // The undergarments the character wears under its armor (shown in game
    // wherever no armor covers them): top (female races) and bottom.
    const std::pair<const char*, uint64_t> undergarments[] = {{"UndergarmentTop", composite::kUndergarmentTopToken},
                                                               {"UndergarmentBottom", composite::kUndergarmentBottomToken}};
    for (const auto& [slot, token] : undergarments) {
        fs::path file = from_utf8(out_dir) / from_utf8(std::string("00_") + slot + ".glb");
        PieceExportResult r = export_undergarment(ctx, token, to_utf8(file));
        if (!r.ok && r.reason.rfind("none for", 0) == 0) continue;  // e.g. no top for male races
        report.push_back({{"slot", slot}, {"status", r.status}, {"file", r.ok ? to_utf8(file.filename()) : std::string()},
                          {"mesh", r.mesh}, {"texture_base", r.texture_base}, {"dyed_channels", 0},
                          {"undyed_channels", r.undyed_channels}, {"reason", r.reason}});
        rep.pieces.emplace_back(slot, std::move(r));
    }

    std::ofstream(from_utf8(out_dir) / "manifest.json") << character::manifest_to_json(manifest).dump(2) << '\n';
    std::ofstream(from_utf8(out_dir) / "export_report.json")
        << json{{"character", manifest.name}, {"race_key", ctx.race_key}, {"pieces", report}}.dump(2) << '\n';
    return rep;
}

} // namespace castlemist::ripper
