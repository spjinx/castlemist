# =====================================================================
# emu_anchors.py -- find the client functions tools/emu depends on, in
# whatever Gw2-64.exe build you have, and name them in Ghidra.
#
# The emu notes (README, docs/research/gw2-net-crypto.md, gw2-login-flow.md)
# were written against one IDB, as sub_14XXXXXXX addresses. Every client patch
# moves them. This script re-derives them from things that survive a patch:
#
#   * string anchors -- a unique string in .rdata (an assert or log message the
#     function prints). The function that loads its address with a
#     rip-relative LEA is the anchor.
#   * signature anchors -- a unique byte pattern in code, like the one gw2patch
#     uses for the RC4 patch site.
#
# A hit is mapped to its function through the exception directory (.pdata),
# not through Ghidra's analysis. So it works on a freshly imported program
# with "Analyze now?" declined, and the same code runs outside Ghidra too.
#
# In Ghidra (PyGhidra; launch with support\pyghidraRun.bat), with Gw2-64.exe
# open: Window > Script Manager, add tools/emu/ghidra/, run emu_anchors.py.
# It creates any missing functions, names them (emu_* names, so they never
# clobber your own), adds a plate comment saying how each was found, and
# writes tools/emu/bin/emu_anchors.json. Headless:
#
#   support\pyghidraRun.bat -H <dir> gw2 -import <...>\Gw2-64.exe -noanalysis
#     -scriptPath <castlemist>\tools\emu\ghidra -postScript emu_anchors.py
#
# Without Ghidra, for a quick check (prints the same JSON):
#
#   python tools/emu/ghidra/emu_anchors.py "<...>\Gw2-64.exe"
# =====================================================================

import json
import os
import struct
import sys

# ---- what to find ---------------------------------------------------------
#
# name: the label to give the function; the rest: how to find it, and what the
# notes called it (for cross-reading docs/research).

STRING_ANCHORS = [
    {"name": "emu_Msg_Raw_ClientRecvEncrypt", "string": "Msg::Raw::ClientRecvEncrypt",
     "note": "MsgConn connect packet -> session key XOR nonce -> RC4 KSA (gw2-net-crypto)"},
    {"name": "emu_MsgConnDispatch", "string": "MsgConnDispatch failed on message %u: %s.",
     "note": "dispatch wrapper around the MsgConn receive path"},
    {"name": "emu_Gc_GcAuthCmdNotifyProc", "string": "Auth failed to connect!",
     "also": ["Connected to auth server %s", "Login server connection lost"],
     "note": "AuthSrv notify proc: 0=fail 1=connected 2/3=disconnect 4=data"},
    {"name": "emu_AccountLoginResult", "string": "Account login transaction result %u",
     "note": "AuthSrv login result handler; result u16 @+36 == 0 is success (gw2-login-flow)"},
    {"name": "emu_PortalUserInfoError", "string": "Error retriving user info from Portal: %u:%u:%u:%u",
     "note": "Portal gate error path (gw2-login-flow)"},
]

SIGNATURE_ANCHORS = [
    {"name": "emu_Rc4Prga", "site": "emu_rc4_identity_site",
     "signature": "0F B6 54 0A 08 49 03 D0 0F B6 C2 0F B6 54 08 08 32 54 3B FF 88 53 FF", "offset": 16,
     "note": "RC4 PRGA; the site is the xor gw2patch turns into a mov (patches.h rc4_identity)"},
]

# ---- a PE image, from a file or from Ghidra's FileBytes ---------------------


class Pe:
    def __init__(self, data):
        self.d = data
        if data[:2] != b"MZ":
            raise ValueError("not a PE file")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("no PE header")
        self.timestamp = struct.unpack_from("<I", data, pe + 8)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", data, opt)[0] != 0x20B:
            raise ValueError("not PE32+ (the 64-bit client is required)")
        self.base = struct.unpack_from("<Q", data, opt + 24)[0]
        # Data directory 3 = exception table (.pdata).
        self.exc_rva, self.exc_size = struct.unpack_from("<II", data, opt + 112 + 3 * 8)
        self.secs = []
        for i in range(nsec):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, opt + optsz + i * 40)
            flags = struct.unpack_from("<I", data, opt + optsz + i * 40 + 36)[0]
            self.secs.append((name.rstrip(b"\0").decode("latin-1"), va, vsize, rptr, rsize, flags))
        self._funcs = None

    def rva_to_off(self, rva):
        for _, va, vsize, rptr, rsize, _ in self.secs:
            if va <= rva < va + min(vsize, rsize):
                return rptr + rva - va
        return None

    def off_to_rva(self, off):
        for _, va, vsize, rptr, rsize, _ in self.secs:
            if rptr <= off < rptr + min(vsize, rsize):
                return va + off - rptr
        return None

    def code_sections(self):
        return [s for s in self.secs if s[5] & 0x20000000]  # IMAGE_SCN_MEM_EXECUTE

    def u32(self, rva):
        return struct.unpack_from("<I", self.d, self.rva_to_off(rva))[0]

    # -- functions from .pdata: RUNTIME_FUNCTION {begin, end, unwind} --------

    def functions(self):
        if self._funcs is None:
            out = []
            off = self.rva_to_off(self.exc_rva)
            for i in range(self.exc_size // 12):
                b, e, u = struct.unpack_from("<III", self.d, off + i * 12)
                if b and e > b:
                    out.append((b, e, u))
            out.sort()
            self._funcs = out
        return self._funcs

    def function_containing(self, rva):
        """(begin, end) of the function holding `rva`. A chained entry (a cold
        or split fragment) is followed back to the function it belongs to."""
        import bisect
        fs = self.functions()
        i = bisect.bisect_right(fs, (rva, 0xFFFFFFFF, 0xFFFFFFFF)) - 1
        if i < 0 or not (fs[i][0] <= rva < fs[i][1]):
            return None
        b, e, u = fs[i]
        for _ in range(16):  # chains are short; bound it anyway
            ui = self.rva_to_off(u & ~1)
            if ui is None:
                break
            flags = self.d[ui] >> 3
            if not flags & 0x4:  # UNW_FLAG_CHAININFO
                break
            count = self.d[ui + 2]
            parent = ui + 4 + 2 * (count + (count & 1))
            b, e, u = struct.unpack_from("<III", self.d, parent)
        return b, e

    # -- searching -------------------------------------------------------

    def find_all(self, needle, sections):
        hits = []
        for _, va, vsize, rptr, rsize, _ in sections:
            end = rptr + min(vsize, rsize)
            i = self.d.find(needle, rptr, end)
            while i >= 0:
                hits.append(va + i - rptr)
                i = self.d.find(needle, i + 1, end)
        return hits

    def find_signature(self, text):
        parts = text.split()
        # Search for the longest run without wildcards, then check the rest.
        best, start = b"", 0
        run, run_start = [], 0
        for i, p in enumerate(parts + ["??"]):
            if p.startswith("?"):
                if len(run) > len(best):
                    best, start = bytes(run), run_start
                run, run_start = [], i + 1
            else:
                run.append(int(p, 16))
        hits = []
        for rva in self.find_all(best, self.code_sections()):
            at = self.rva_to_off(rva - start)
            ok = at is not None and all(
                p.startswith("?") or self.d[at + k] == int(p, 16) for k, p in enumerate(parts))
            if ok:
                hits.append(rva - start)
        return hits

    def lea_refs(self, target_rva):
        """RVAs of rip-relative LEAs (48/4C 8D modrm[mod=0,rm=5] disp32) that
        load `target_rva`. Scans every code byte; ~2 s on the 43 MB client."""
        refs = []
        for _, va, vsize, rptr, rsize, _ in self.code_sections():
            end = rptr + min(vsize, rsize) - 7
            for rex in (b"\x48\x8d", b"\x4c\x8d"):
                i = self.d.find(rex, rptr, end)
                while i >= 0:
                    modrm = self.d[i + 2]
                    if modrm & 0xC7 == 0x05:
                        disp = struct.unpack_from("<i", self.d, i + 3)[0]
                        ins = va + i - rptr
                        if ins + 7 + disp == target_rva:
                            refs.append(ins)
                    i = self.d.find(rex, i + 1, end)
        return refs


# ---- the resolution ---------------------------------------------------------


def resolve(pe):
    data_secs = [s for s in pe.secs if not s[5] & 0x20000000]
    result = {"peTimestamp": pe.timestamp, "imagebase": "0x%X" % pe.base, "anchors": []}

    def func_of(rva):
        f = pe.function_containing(rva)
        return None if f is None else f[0]

    for a in STRING_ANCHORS:
        entry = {"name": a["name"], "found_by": "string", "anchor": a["string"], "note": a["note"]}
        strs = pe.find_all(a["string"].encode() + b"\0", data_secs)
        if len(strs) != 1:
            entry["error"] = "string found %d times (need exactly 1)" % len(strs)
            result["anchors"].append(entry)
            continue
        funcs = sorted({f for f in (func_of(r) for r in pe.lea_refs(strs[0])) if f is not None})
        # Corroborating strings must land in the same function.
        for extra in a.get("also", []):
            s2 = pe.find_all(extra.encode() + b"\0", data_secs)
            got = sorted({f for s in s2 for f in (func_of(r) for r in pe.lea_refs(s)) if f is not None})
            entry.setdefault("corroborated_by", []).append(
                {"string": extra, "agrees": bool(funcs) and got == funcs})
        if len(funcs) == 1:
            entry["va"] = "0x%X" % (pe.base + funcs[0])
        else:
            entry["error"] = "referenced from %d functions: %s" % (
                len(funcs), ", ".join("0x%X" % (pe.base + f) for f in funcs) or "none")
        result["anchors"].append(entry)

    for a in SIGNATURE_ANCHORS:
        entry = {"name": a["name"], "found_by": "signature", "anchor": a["signature"], "note": a["note"]}
        hits = pe.find_signature(a["signature"])
        if len(hits) != 1:
            entry["error"] = "signature matched %d times (need exactly 1)" % len(hits)
        else:
            site = hits[0] + a["offset"]
            entry["site"] = {"name": a["site"], "va": "0x%X" % (pe.base + site)}
            f = func_of(site)
            if f is None:
                entry["error"] = "site is in no .pdata function"
            else:
                entry["va"] = "0x%X" % (pe.base + f)
        result["anchors"].append(entry)
    return result


# ---- Ghidra: read the original file bytes, then name what was found ---------


def _ghidra_bytes():
    import jpype
    mem = currentProgram.getMemory()  # noqa: F821 (Ghidra script global)
    fbs = list(mem.getAllFileBytes())
    if not fbs:
        raise RuntimeError("the program has no original file bytes (imported from something other than a file?)")
    fb = fbs[0]
    size = int(fb.getSize())
    raw = jpype.JArray(jpype.JByte)(size)
    fb.getOriginalBytes(jpype.JLong(0), raw)
    try:
        return memoryview(raw).cast("B").tobytes()  # JPype exposes primitive arrays as buffers
    except (TypeError, ValueError):
        return bytes(b & 0xFF for b in raw)


def _ghidra_apply(result):
    from ghidra.program.model.symbol import SourceType
    from ghidra.program.model.listing import CodeUnit
    import jpype
    prog = currentProgram  # noqa: F821
    space = prog.getAddressFactory().getDefaultAddressSpace()
    listing = prog.getListing()
    fm = prog.getFunctionManager()

    def addr(text):
        return space.getAddress(jpype.JLong(int(text, 16)))

    for a in result["anchors"]:
        if "va" not in a:
            continue
        at = addr(a["va"])
        f = fm.getFunctionAt(at)
        if f is None:
            disassemble(at)  # noqa: F821
            f = createFunction(at, a["name"])  # noqa: F821
        if f is not None:
            f.setName(a["name"], SourceType.USER_DEFINED)
            listing.setComment(at, CodeUnit.PLATE_COMMENT,
                               "%s\nFound by %s: %s\n(tools/emu/ghidra/emu_anchors.py)" % (
                                   a["note"], a["found_by"], a["anchor"]))
        if "site" in a:
            createLabel(addr(a["site"]["va"]), a["site"]["name"], True)  # noqa: F821


def _out_path():
    here = None
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        try:
            here = str(getSourceFile().getParentFile().getAbsolutePath())  # noqa: F821
        except Exception:
            pass
    if here:
        bin_dir = os.path.normpath(os.path.join(here, "..", "bin"))
        os.makedirs(bin_dir, exist_ok=True)
        return os.path.join(bin_dir, "emu_anchors.json")
    return os.path.join(os.path.expanduser("~"), "emu_anchors.json")


def _report(result):
    ok = sum(1 for a in result["anchors"] if "va" in a)
    print("emu anchors: %d of %d found (client PE timestamp %d)" % (ok, len(result["anchors"]), result["peTimestamp"]))
    for a in result["anchors"]:
        line = "  %-32s %s" % (a["name"], a.get("va") or "NOT FOUND: " + a.get("error", "?"))
        if "site" in a:
            line += "   site %s" % a["site"]["va"]
        for c in a.get("corroborated_by", []):
            line += "   [%s: %s]" % (c["string"][:24], "agrees" if c["agrees"] else "DISAGREES")
        print(line)


def main_ghidra():
    result = resolve(Pe(_ghidra_bytes()))
    _ghidra_apply(result)
    path = _out_path()
    with open(path, "w") as f:
        json.dump(result, f, indent=2)
    _report(result)
    print("Wrote " + path)


def main_cli(argv):
    if len(argv) < 2:
        print("usage: emu_anchors.py <Gw2-64.exe> [out.json]")
        return 2
    with open(argv[1], "rb") as f:
        result = resolve(Pe(f.read()))
    text = json.dumps(result, indent=2)
    if len(argv) > 2:
        with open(argv[2], "w") as f:
            f.write(text)
    _report(result)
    return 0


def _in_ghidra():
    # PyGhidra provides the script API (currentProgram, ...) as names that are
    # resolvable but not necessarily in globals(), so probe rather than look.
    try:
        currentProgram  # noqa: F821,B018
        return True
    except NameError:
        return False


if _in_ghidra():
    main_ghidra()
elif __name__ == "__main__":
    sys.exit(main_cli(sys.argv))
