// pe_patch.h - minimal PE64 VA<->file-offset mapping + patch application.
// Target: Gw2-64-disable-aslr.exe (imagebase 0x140000000, ASLR off).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gw2pe {

struct Section {
    char     name[9];
    uint32_t vaddr;      // VirtualAddress (RVA)
    uint32_t vsize;      // VirtualSize
    uint32_t raw_ptr;    // PointerToRawData (file offset)
    uint32_t raw_size;   // SizeOfRawData
    uint32_t flags;      // Characteristics (IMAGE_SCN_*)

    bool executable() const { return (flags & 0x20000000u) != 0; }  // IMAGE_SCN_MEM_EXECUTE
};

// A byte pattern with wildcards, written the way disassemblers print bytes:
// "0F B6 54 0A ?? 49". Spaces are optional; "??" (or "?") matches any byte.
struct Signature {
    std::vector<uint8_t> bytes;
    std::vector<bool>    any;   // true where the pattern has a wildcard

    static bool parse(const char* text, Signature& out) {
        out = Signature{};
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (const char* p = text; *p;) {
            if (*p == ' ') { ++p; continue; }
            if (*p == '?') {
                out.bytes.push_back(0);
                out.any.push_back(true);
                p += (p[1] == '?') ? 2 : 1;
                continue;
            }
            int hi = hex(p[0]), lo = p[1] ? hex(p[1]) : -1;
            if (hi < 0 || lo < 0) return false;
            out.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
            out.any.push_back(false);
            p += 2;
        }
        return !out.bytes.empty();
    }
};

class Image {
public:
    bool load(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf_.resize(sz);
        size_t rd = fread(buf_.data(), 1, sz, f);
        fclose(f);
        if (rd != (size_t)sz) return false;
        return parse();
    }

    bool save(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        size_t wr = fwrite(buf_.data(), 1, buf_.size(), f);
        fclose(f);
        return wr == buf_.size();
    }

    uint64_t imagebase() const { return imagebase_; }

    // Virtual address -> file offset. Returns SIZE_MAX if unmapped.
    size_t va_to_off(uint64_t va) const {
        if (va < imagebase_) return SIZE_MAX;
        uint32_t rva = (uint32_t)(va - imagebase_);
        for (const auto& s : sections_) {
            if (rva >= s.vaddr && rva < s.vaddr + s.vsize) {
                uint32_t d = rva - s.vaddr;
                if (d >= s.raw_size) return SIZE_MAX; // in virtual-only tail
                return s.raw_ptr + d;
            }
        }
        return SIZE_MAX;
    }

    // Read/patch bytes at a VA. Returns false if unmapped or verify mismatch.
    bool read_at(uint64_t va, uint8_t* out, size_t n) const {
        size_t off = va_to_off(va);
        if (off == SIZE_MAX || off + n > buf_.size()) return false;
        memcpy(out, &buf_[off], n);
        return true;
    }

    // Apply a patch, verifying the current bytes match `expect` first.
    // expect/replace equal length. Pass empty `expect` to skip verification.
    bool patch_at(uint64_t va, const std::vector<uint8_t>& expect,
                  const std::vector<uint8_t>& replace, std::string& err) {
        size_t off = va_to_off(va);
        if (off == SIZE_MAX || off + replace.size() > buf_.size()) {
            err = "VA unmapped or out of range";
            return false;
        }
        if (!expect.empty()) {
            if (expect.size() != replace.size()) { err = "expect/replace length mismatch"; return false; }
            if (memcmp(&buf_[off], expect.data(), expect.size()) != 0) {
                err = "current bytes != expected (wrong build or already patched)";
                return false;
            }
        }
        memcpy(&buf_[off], replace.data(), replace.size());
        return true;
    }

    // File offset -> virtual address. Returns 0 if the offset is in no section.
    uint64_t off_to_va(size_t off) const {
        for (const auto& s : sections_) {
            if (off >= s.raw_ptr && off < static_cast<size_t>(s.raw_ptr) + s.raw_size &&
                off - s.raw_ptr < s.vsize)
                return imagebase_ + s.vaddr + (off - s.raw_ptr);
        }
        return 0;
    }

    // Every match of `sig` inside the executable sections, as file offsets.
    // Code only: the same bytes in .rdata or a resource are never a patch site.
    std::vector<size_t> find_in_code(const Signature& sig) const {
        std::vector<size_t> hits;
        const size_t n = sig.bytes.size();
        for (const auto& s : sections_) {
            if (!s.executable()) continue;
            const size_t begin = s.raw_ptr;
            const size_t end = std::min<size_t>(buf_.size(), static_cast<size_t>(s.raw_ptr) +
                                                                 std::min(s.raw_size, s.vsize));
            for (size_t i = begin; i + n <= end; ++i) {
                size_t k = 0;
                while (k < n && (sig.any[k] || buf_[i + k] == sig.bytes[k])) ++k;
                if (k == n) hits.push_back(i);
            }
        }
        return hits;
    }

    bool bytes_at_off(size_t off, const std::vector<uint8_t>& want) const {
        return off + want.size() <= buf_.size() && memcmp(&buf_[off], want.data(), want.size()) == 0;
    }

    // Search the whole file for a byte pattern; returns file offsets.
    std::vector<size_t> find(const std::vector<uint8_t>& pat) const {
        std::vector<size_t> hits;
        if (pat.empty() || pat.size() > buf_.size()) return hits;
        for (size_t i = 0; i + pat.size() <= buf_.size(); ++i) {
            if (memcmp(&buf_[i], pat.data(), pat.size()) == 0) hits.push_back(i);
        }
        return hits;
    }

    bool patch_file_off(size_t off, const std::vector<uint8_t>& replace) {
        if (off + replace.size() > buf_.size()) return false;
        memcpy(&buf_[off], replace.data(), replace.size());
        return true;
    }

private:
    bool parse() {
        if (buf_.size() < 0x40 || buf_[0] != 'M' || buf_[1] != 'Z') return false;
        uint32_t pe = rd32(0x3C);
        if (pe + 24 > buf_.size() || memcmp(&buf_[pe], "PE\0\0", 4) != 0) return false;
        uint16_t nsec = rd16(pe + 6);
        uint16_t opt_sz = rd16(pe + 20);
        uint32_t opt = pe + 24;
        uint16_t magic = rd16(opt);
        if (magic != 0x20b) return false;              // PE32+ only
        imagebase_ = rd64(opt + 24);
        uint32_t sec = opt + opt_sz;
        for (uint16_t i = 0; i < nsec; ++i) {
            uint32_t s = sec + i * 40;
            Section sc{};
            memcpy(sc.name, &buf_[s], 8); sc.name[8] = 0;
            sc.vsize   = rd32(s + 8);
            sc.vaddr   = rd32(s + 12);
            sc.raw_size= rd32(s + 16);
            sc.raw_ptr = rd32(s + 20);
            sc.flags   = rd32(s + 36);
            sections_.push_back(sc);
        }
        return true;
    }
    uint16_t rd16(size_t o) const { return buf_[o] | (buf_[o+1] << 8); }
    uint32_t rd32(size_t o) const { return (uint32_t)buf_[o] | ((uint32_t)buf_[o+1]<<8)
                                         | ((uint32_t)buf_[o+2]<<16) | ((uint32_t)buf_[o+3]<<24); }
    uint64_t rd64(size_t o) const { return (uint64_t)rd32(o) | ((uint64_t)rd32(o+4) << 32); }

    std::vector<uint8_t> buf_;
    std::vector<Section> sections_;
    uint64_t imagebase_ = 0;
};

} // namespace gw2pe
