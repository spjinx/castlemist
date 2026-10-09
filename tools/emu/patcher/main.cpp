// gw2patch - apply the client-only-emulator patches to a COPY of the GW2 exe.
//
//   gw2patch <input.exe> <output.exe>
//
// Never patch the live client in place; always work on a copy. This tool:
//   - finds each byte patch (at its recorded VA, else by its signature),
//     verifies the bytes, and applies it (RC4 -> identity)
//   - best-effort string-replaces the auth endpoint constants -> 127.0.0.1
//   - reports what it did, so mismatches (wrong build) are obvious.
#include "pe_patch.h"
#include "patches.h"
#include <cstdio>
#include <string>

using namespace gw2pe;

static std::vector<uint8_t> str_bytes(const char* s) {
    return std::vector<uint8_t>(s, s + strlen(s));
}

// Where a byte patch goes in this image.
struct Site {
    enum Kind { Found, AlreadyPatched, Failed } kind = Failed;
    size_t off = 0;           // file offset of the patched bytes
    const char* how = "";     // "table VA" / "signature"
    std::string err;
};

static Site locate(const Image& img, const BytePatch& p) {
    Site s;
    // 1) The recorded VA, when this is the build it was recorded on.
    const size_t va_off = img.va_to_off(p.va);
    if (va_off != SIZE_MAX) {
        if (img.bytes_at_off(va_off, p.expect)) return {Site::Found, va_off, "table VA", ""};
        if (img.bytes_at_off(va_off, p.replace)) return {Site::AlreadyPatched, va_off, "table VA", ""};
    }
    if (!p.signature) {
        s.err = "bytes at the table VA differ (different client build) and the patch has no signature";
        return s;
    }
    // 2) The signature, anywhere in the code. It must match exactly once: two
    //    hits means the pattern is no longer specific enough to trust.
    Signature sig;
    if (!Signature::parse(p.signature, sig) || p.sig_offset + p.expect.size() > sig.bytes.size()) {
        s.err = "malformed signature in the patch table";
        return s;
    }
    std::vector<size_t> hits = img.find_in_code(sig);
    // A copy that was already patched no longer matches the original pattern,
    // so look for the patched form too.
    Signature done = sig;
    for (size_t i = 0; i < p.replace.size(); ++i) {
        done.bytes[p.sig_offset + i] = p.replace[i];
        done.any[p.sig_offset + i] = false;
    }
    std::vector<size_t> done_hits = img.find_in_code(done);
    if (hits.size() == 1 && done_hits.empty()) {
        const size_t off = hits[0] + p.sig_offset;
        if (!img.bytes_at_off(off, p.expect)) {
            s.err = "signature matched but the bytes to replace differ";
            return s;
        }
        return {Site::Found, off, "signature", ""};
    }
    if (hits.empty() && done_hits.size() == 1)
        return {Site::AlreadyPatched, done_hits[0] + p.sig_offset, "signature", ""};
    s.err = "signature matched " + std::to_string(hits.size()) + " time(s) (" +
            std::to_string(done_hits.size()) + " already patched); expected exactly 1";
    return s;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("usage: %s <input.exe> <output.exe>\n", argv[0]);
        printf("  applies the plaintext-emulator patches to a COPY of the client.\n");
        return 2;
    }
    Image img;
    if (!img.load(argv[1])) {
        printf("[!] failed to load '%s'\n", argv[1]);
        return 1;
    }
    printf("[*] loaded %s  (imagebase 0x%llx)\n", argv[1],
           (unsigned long long)img.imagebase());

    int applied = 0, failed = 0;

    // --- concrete byte patches -------------------------------------------
    for (const auto& p : byte_patches()) {
        Site s = locate(img, p);
        const unsigned long long va = (unsigned long long)img.off_to_va(s.off);
        switch (s.kind) {
        case Site::Found:
            img.patch_file_off(s.off, p.replace);
            printf("[+] %-16s @ 0x%llx  ok, by %s  (%s)\n", p.name, va, s.how, p.detail);
            if (va != p.va)
                printf("    (the table says 0x%llx: this is a different client build)\n",
                       (unsigned long long)p.va);
            ++applied;
            break;
        case Site::AlreadyPatched:
            printf("[=] %-16s @ 0x%llx  already applied (found by %s)\n", p.name, va, s.how);
            break;
        case Site::Failed:
            printf("[%c] %-16s  FAILED: %s\n", p.required ? '!' : '~', p.name, s.err.c_str());
            if (p.required) ++failed;
            break;
        }
    }

    // --- best-effort endpoint string replacement -------------------------
    for (const auto& sp : endpoint_strings()) {
        auto needle = str_bytes(sp.find);
        auto hits = img.find(needle);
        if (hits.empty()) {
            printf("[~] endpoint     '%s' not found (skipped)\n", sp.find);
            continue;
        }
        std::vector<uint8_t> rep(needle.size(), 0);          // NUL-pad to keep length
        size_t rl = strlen(sp.replace);
        if (rl > needle.size()) { printf("[!] replacement too long for '%s'\n", sp.find); continue; }
        memcpy(rep.data(), sp.replace, rl);
        for (size_t off : hits) img.patch_file_off(off, rep);
        printf("[+] endpoint     '%s' -> '%s'  (%zu site(s))\n", sp.find, sp.replace, hits.size());
        ++applied;
    }

    if (failed) {
        printf("[!] %d required patch(es) failed - NOT writing output.\n", failed);
        return 1;
    }
    if (!img.save(argv[2])) {
        printf("[!] failed to write '%s'\n", argv[2]);
        return 1;
    }
    printf("[*] wrote %s  (%d patch group(s) applied)\n", argv[2], applied);
    printf("[i] reminder: portal->AuthSrv trigger (patch #4) still TODO; see patches.h\n");
    return 0;
}
