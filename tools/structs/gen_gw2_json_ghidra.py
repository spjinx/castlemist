# =====================================================================
# gen_gw2_json_ghidra.py -- Ghidra port of gen_gw2_json.py, for anyone stuck
# on IDA Free (which does not include IDAPython -- Python scripting is an
# IDA Home/Pro-only feature; there is no configuration workaround for that).
#
# Same output schema, same algorithm, ported line-for-line from the IDA
# version: scan the loaded image for GW2's own "chunk_info" reflection
# tables (a real in-memory C struct the client carries so it can name every
# packfile chunk's fields at runtime) and dump them as JSON. No decompiler,
# no function analysis needed -- this only reads raw bytes/pointers, so you
# can decline Ghidra's "Analyze now?" prompt on import and this will still
# work; that dialog exists for disassembly/decompilation, neither of which
# this script touches.
#
# Run: launch Ghidra via support/pyghidraRun.bat (NOT the normal ghidraRun
# shortcut -- this needs PyGhidra's native-CPython3 scripting, which recent
# Ghidra only enables through that launcher; the old Jython Script Manager
# will refuse a .py script with "Ghidra was not started with PyGhidra").
# Then: Window > Script Manager -> add tools/structs/ as a script directory
# if it isn't already listed -> double-click this script with your
# Gw2-64.exe CodeBrowser open and active.
#
# UNTESTED against a real Gw2-64.exe -- there is no GW2 install available
# where this was written, only the IDA original to port from. The logic is
# a careful 1:1 translation (see the comments matching each function back to
# gen_gw2_json.py), but "translates cleanly" is not the same as "verified
# against real data". If a run produces zero fileTypes/chunks, the most
# likely culprits, in order: (1) this exe's real section name isn't exactly
# ".rdata" (some builds fold read-only data into ".text" -- the "if RD0==0"
# fallback here mirrors the IDA original's own fallback for that), (2) the
# reflection tables genuinely moved/changed shape in a client patch, (3) a
# bug in this port. Compare a suspicious result against dumps/packfile/
# gw2_packfile.json's existing shape (e.g. does "mach" still show up in
# strucTabs with 2 PackAnimMachines versions?) before assuming (2) or (3).
#
# Output schema (unchanged from gen_gw2_json.py):
#   { "format":"gw2packfile", "pointerSize":64,
#     "fileTypes": {...}, "chunks": {...}, "strucTabs": {...}, "types": {...} }
# =====================================================================

import json
import jpype
from ghidra.program.model.mem import MemoryAccessException

OUT_PATH = r"C:\Users\vital\Downloads\castlemist\castlemist\dumps\packfile\gw2_packfile.json"

mem = currentProgram.getMemory()
BADADDR = 0xFFFFFFFFFFFFFFFF

# toAddr() has both an int- and a long-taking Java overload; JPype can
# resolve a plain Python int to the 32-bit one and then choke on any address
# above 0x7FFFFFFF ("Cannot convert value to Java int"), which is every
# address here (imagebase alone is 0x140000000). Going straight through the
# address space's own getAddress(long) sidesteps the overload entirely.
_addrSpace = currentProgram.getAddressFactory().getDefaultAddressSpace()
def toA(a): return _addrSpace.getAddress(jpype.JLong(a))

# ---- raw reads anywhere in the loaded image (mirrors ida_bytes.get_*) ------
#
# IDA's get_byte/get_word/... return a safe sentinel for unmapped addresses
# instead of raising, which the original script leans on implicitly (wild
# pointers during struct-walking just read as garbage and fail a later
# range/shape check rather than crashing the scan). Ghidra's Memory throws
# MemoryAccessException on an unmapped read, so that's caught here and
# turned into the same kind of "obviously invalid" sentinel.

def u8(a):
    try: return mem.getByte(toA(a)) & 0xFF
    except MemoryAccessException: return 0xFF

def u16(a):
    try: return mem.getShort(toA(a)) & 0xFFFF
    except MemoryAccessException: return 0xFFFF

def u32(a):
    try: return mem.getInt(toA(a)) & 0xFFFFFFFF
    except MemoryAccessException: return 0xFFFFFFFF

def u64(a):
    try: return mem.getLong(toA(a)) & 0xFFFFFFFFFFFFFFFF
    except MemoryAccessException: return BADADDR

# ---- section lookup (mirrors ida_segment.get_segm_by_name) -----------------

def get_block(name):
    for b in mem.getBlocks():
        if b.getName() == name:
            return b
    return None

def slurp(name):
    """(startOffset, bytearray) for one memory block, read in one call
    rather than one Ghidra API call per byte -- the scan loop below walks
    every 4-byte-aligned offset across the whole section, which is millions
    of addresses for a section this size; doing that through toAddr()+
    getByte() per address is the difference between seconds and an hour.
    PyGhidra runs on JPype, not Jython, so the Java byte[] out-parameter
    Memory.getBytes() needs is built via jpype.JArray, not Jython's jarray."""
    b = get_block(name)
    if b is None or not b.isInitialized():
        return None
    size = int(b.getSize())
    raw = jpype.JArray(jpype.JByte)(size)
    mem.getBytes(b.getStart(), raw)  # Memory.getBytes, not MemoryBlock's (version-safer)
    buf = bytearray(size)  # Java bytes are signed; mask each one to 0-255
    for i in range(size):
        buf[i] = raw[i] & 0xFF
    return b.getStart().getOffset(), buf

_got = slurp(".rdata")
if _got is None:
    _got = slurp(".text")
if _got is None:
    raise RuntimeError("neither .rdata nor .text found/initialized in this program")
RD0, _buf = _got
RD1 = RD0 + len(_buf)

# ---- byte classification / C-string read (mirrors is_ascii/cstr) ----------

def is_ascii(b): return (48 <= b <= 57) or (65 <= b <= 90) or (97 <= b <= 122)

def cstr(a, m=128):
    if not a: return ""
    o = []
    for _ in range(m):
        b = u8(a)
        if b == 0: break
        o.append(chr(b)); a += 1
    return "".join(o)

# ---- field-kind tables (verbatim from gen_gw2_json.py) ---------------------

SIMPLE = {
    0x05: "byte", 0x06: "byte4", 0x07: "double", 0x0A: "dword", 0x0B: "filename",
    0x0C: "float", 0x0D: "float2", 0x0E: "float3", 0x0F: "float4", 0x11: "qword",
    0x12: "wchar_ptr", 0x13: "char_ptr", 0x15: "word", 0x16: "byte16", 0x17: "byte3",
    0x18: "dword2", 0x19: "dword4", 0x1A: "word3", 0x1B: "fileref",
    0x1C: "dword",  # variant 0x1C (unconfirmed; 4B like dword)
    0x24: "token32", 0x25: "token64",
}
CHILD = {0x01, 0x02, 0x03, 0x10, 0x14, 0x1D}

# ---- struct-descriptor validation (mirrors is_anstruct/is_anstructtab) -----

def is_anstruct(a):
    g, c = 50, a
    while u16(c) != 0 and g > 0:
        if u16(c) > 0x1D: return False
        c += 32; g -= 1
    if g == 0: return False
    p = u64(c + 8)
    return p not in (0, BADADDR) and is_ascii(u8(p))

def is_anstructtab(a, n):
    c = a
    for _ in range(n):
        p = u64(c)
        if p != 0 and not is_anstruct(p): return False
        c += 24
    return True

types = {}      # key -> {"fields":[...]}
addr2key = {}   # descriptor addr -> key
name2addr = {}  # name -> addr (collision detection)

def struct_name(desc):
    c = desc
    while u16(c) != 0: c += 32
    return cstr(u64(c + 8))

def is_simple_wrapper(desc):
    np = u64(desc + 8)
    return np == 0 or u8(np) == 0

def elem_of(child):
    if child == 0: return "dword"
    if is_simple_wrapper(child):
        return SIMPLE.get(u16(child), "dword")
    return {"struct": build_struct(child)}

def member(entry):
    tid = u16(entry); nm = cstr(u64(entry + 8)); child = u64(entry + 16); cnt = u64(entry + 24)
    if tid == 0x01: return {"name": nm, "kind": "array", "element": elem_of(child), "count": int(cnt)}
    if tid == 0x02: return {"name": nm, "kind": "array_ptr", "element": elem_of(child)}
    if tid == 0x03: return {"name": nm, "kind": "ptr_array_ptr", "element": elem_of(child)}
    if tid == 0x10: return {"name": nm, "kind": "ptr", "target": elem_of(child)}
    if tid in (0x14, 0x1D):
        e = elem_of(child)
        if isinstance(e, dict): return {"name": nm, "kind": "struct", "type": e["struct"]}
        return {"name": nm, "kind": e}
    return {"name": nm, "kind": SIMPLE.get(tid, "dword")}

def build_struct(desc):
    if desc in addr2key: return addr2key[desc]
    name = struct_name(desc) or ("Anon_%X" % desc)
    key = name
    if name in name2addr and name2addr[name] != desc:
        key = "%s_%X" % (name, desc & 0xFFFFF)  # disambiguate name collisions
    else:
        name2addr[name] = desc
    addr2key[desc] = key
    types[key] = {"fields": []}  # register first to break recursion
    fields = []
    c = desc
    while u16(c) != 0:
        fields.append(member(c)); c += 32
    types[key]["fields"] = fields
    return key

CHUNK_INFO_STRIDE = 16  # {char name[4]; u32 nVersions; u64 strucTab}

def build_vmap(nver, tab):
    vmap = {}
    for i in range(nver):
        desc = u64(tab + 24 * i)
        if desc: vmap[str(i)] = build_struct(desc)
    return vmap

def main():
    # 1) collect every chunk_info candidate (no dedup), with its address.
    # This is the hot loop -- millions of 4-byte-aligned offsets across the
    # whole section -- so it reads straight out of the pre-slurped buffer
    # instead of going through toAddr()/getByte() per byte.
    infos = []  # (addr, name, nver, tab)
    n = len(_buf)
    off = 0
    while off < n - 16:
        b0, b1, b2, b3 = _buf[off], _buf[off + 1], _buf[off + 2], _buf[off + 3]
        if is_ascii(b0) and is_ascii(b1) and is_ascii(b2) and (b3 == 0 or is_ascii(b3)):
            name = chr(b0) + chr(b1) + chr(b2) + (chr(b3) if b3 else "")
            a = RD0 + off
            nver = u32(a + 4)
            if 0 < nver < 100:
                tab = u64(a + 8)
                if RD0 < tab < RD1 and is_anstructtab(tab, nver):
                    infos.append((a, name, nver, tab))
        off += 4
    infos.sort()

    # 2) group into contiguous stride-16 runs = one file-type per run.
    groups = []; cur = []; prev = None
    for addr, name, nver, tab in infos:
        if prev is not None and addr - prev != CHUNK_INFO_STRIDE:
            if cur: groups.append(cur)
            cur = []
        cur.append((name, nver, tab)); prev = addr
    if cur: groups.append(cur)

    # 3) fileTypes[containerFourcc][chunkFourcc][ver] = typeKey (merge like runs)
    fileTypes = {}
    for g in groups:
        ftKey = g[0][0]  # first fourcc in the run = the container/file type
        ft = fileTypes.setdefault(ftKey, {})
        for name, nver, tab in g:
            vmap = build_vmap(nver, tab)
            if vmap: ft.setdefault(name, {}).update(vmap)

    # 4) global fallback: only fourccs with EXACTLY ONE strucTab (unambiguous)
    tabs_per_name = {}
    for _, name, _, tab in infos:
        tabs_per_name.setdefault(name, set()).add(tab)
    chunks = {}
    for name, nver, tab in {(n_, v, t) for _, n_, v, t in infos}:
        if len(tabs_per_name[name]) == 1:
            vmap = build_vmap(nver, tab)
            if vmap: chunks[name] = vmap

    # 5) strucTabs[fourcc] = every distinct strucTab seen for that fourcc
    usedby = {}  # name -> tab -> set(fileType)
    for g in groups:
        ftKey = g[0][0]
        for name, nver, tab in g:
            usedby.setdefault(name, {}).setdefault(tab, set()).add(ftKey)
    nver_of = {}  # (name,tab) -> nver
    for _, name, nver, tab in infos:
        nver_of[(name, tab)] = max(nver_of.get((name, tab), 0), nver)
    strucTabs = {}
    for name in usedby:
        lst = []
        for tab in sorted(usedby[name]):
            nv = nver_of[(name, tab)]
            vmap = build_vmap(nv, tab)
            lst.append({"tab": "0x%X" % tab, "nver": nv,
                       "usedBy": sorted(usedby[name][tab]), "versions": vmap})
        strucTabs[name] = lst

    doc = {"format": "gw2packfile", "pointerSize": 64,
          "fileTypes": fileTypes, "chunks": chunks, "strucTabs": strucTabs, "types": types}
    with open(OUT_PATH, "w") as f:
        json.dump(doc, f, separators=(",", ":"))
    amb = sorted(nm for nm, t in tabs_per_name.items() if len(t) > 1)
    print("OK fileTypes=%d chunks=%d types=%d ambiguous=%s" % (
        len(fileTypes), len(chunks), len(types), amb))

main()
