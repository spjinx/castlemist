"""Recover MSVC C++ class names from RTTI and name every vftable in one pass.

Unlike ida_restore_symbols.py's prefix-matched backup -- which can only
restore names for functions someone already found and named by hand -- this
walks the compiler-generated RTTI that MSVC still emits for every
polymorphic class, even when every function in the binary is stripped.
Gw2-64.exe's engine is a deeply class-based C++ codebase, so one pass here
turns hundreds of otherwise-anonymous vftables into real class names for
free, with no prior identification needed.

Ported (as an x64 IDAPython rewrite, not a straight port -- see below) from
the classic 32-bit `parseRTTI.idc` technique circulated in the RE community
and also present, unmodified for x86, in R-Hidayatullah/gw2-utility. That
version cannot be pointed at Gw2-64.exe as-is: x86 and x64 use different
RTTICompleteObjectLocator layouts (see below), and the game only ships a
64-bit client.

Run inside IDA:  File > Script file... > ida_apply_rtti_names.py
or headless:     idat64 -A -S"ida_apply_rtti_names.py" Gw2-64.exe.i64

MSVC x64 RTTI layout (all pointer fields are 4-byte RVAs from the image
base, unlike x86's absolute pointers -- the reason this needed a rewrite
rather than a port):

    RTTICompleteObjectLocator (24 bytes, one per vftable):
        u32 signature       1 on x64 (0 on x86 -- what the x86 script keys on instead)
        u32 offset          this-adjustment to the vfptr within the object;
                             0 for a class's primary (first) vtable
        u32 cdOffset
        u32 pTypeDescriptor   RVA
        u32 pClassDescriptor  RVA (RTTIClassHierarchyDescriptor -- not walked here)
        u32 pSelf             RVA of this COL itself

    pSelf is the load-independent cross-check this script relies on: a
    candidate is accepted only if imagebase + pSelf == the candidate's own
    address, which false-positives on essentially no random 4-byte-aligned
    data. That means .rdata can be scanned blind, with no prior symbols and
    no xrefs, and still get a near-zero false-positive rate.

    RTTITypeDescriptor:
        u64 pVFTable    type_info's own vtable; only range-checked here
        u64 spare       0 for every ordinary class
        char name[]     mangled: ".?AVClassName@@" (class) or ".?AUName@@" (struct)

    Memory layout: [pointer to COL][vftable slot 0][vftable slot 1]...
    i.e. the COL address sits exactly 8 bytes before the vtable it
    describes -- found here by indexing every qword in .rdata by value and
    looking up each validated COL address, rather than relying on IDA's
    xref engine (which has nothing to offer over data nobody has typed yet).

Naming: for the common case (COL.offset == 0, single inheritance or a
class's primary vtable) this mints the exact symbol MSVC itself would have
emitted -- "??_7ClassName@@6B@" for the vtable, "??_R4ClassName@@6B@" for
the COL, "??_R0?AVClassName@@@8" for the type descriptor -- so IDA's own
demangler displays them as "const ClassName::`vftable'" etc. automatically.
Secondary vtables (COL.offset != 0, from multiple inheritance) get a
friendly non-mangled fallback name instead of a guessed mangled one, since
reproducing MSVC's mangling there needs the base class's name too (from
RTTIClassHierarchyDescriptor / RTTIBaseClassArray, which this script does
not walk). The class name is still recovered correctly either way -- only
the symbol's exact mangled spelling is affected.

Writes tools/gw2_rtti_classes.json (vtable address -> recovered class),
a side-car in the same spirit as gw2_ida_symbols.json, so the class list
survives outside the .i64 and can be diffed after a client patch.
"""

import json
import os
import re

import ida_bytes
import ida_ida
import ida_name
import ida_nalt
import ida_segment
import idautils
import idc


def _imagebase():
    """_imagebase() was removed in IDA 9.0."""
    for getter in (getattr(ida_ida, "inf_get_min_ea", None),
                   getattr(ida_ida, "get_imagebase", None)):
        if getter:
            try:
                return getter()
            except Exception:
                pass
    return ida_nalt.get_imagebase()


def _max_ea():
    for getter in (getattr(ida_ida, "inf_get_max_ea", None),):
        if getter:
            try:
                return getter()
            except Exception:
                pass
    return idc.get_inf_attr(idc.INF_MAX_EA)


HERE = os.path.dirname(os.path.abspath(__file__))
JSON_PATH = os.path.join(HERE, "gw2_rtti_classes.json")

_BAD_NAME_CHARS = re.compile(r"[^A-Za-z0-9_$?@]")


def _require_64bit():
    try:
        is64 = ida_ida.inf_is_64bit()
    except Exception:
        is64 = True  # API surface differs across IDA versions; fail open
    if not is64:
        print("!! ida_apply_rtti_names targets Gw2-64.exe's x64 COL layout; "
              "this IDB looks 32-bit and would silently find nothing.")
        return False
    return True


def _rdata_segments():
    """.rdata (and any .rdata$xx sub-segments) -- where MSVC puts both RTTI
    and vtables. Falls back to every DATA/CONST-class segment if the binary
    was built with unusual section names."""
    segs = [s for s in map(ida_segment.getseg, idautils.Segments())
            if s and "rdata" in (ida_segment.get_segm_name(s) or "").lower()]
    if segs:
        return segs
    return [s for s in map(ida_segment.getseg, idautils.Segments())
            if s and ida_segment.get_segm_class(s) in ("DATA", "CONST")]


def _build_ptr_index(segs):
    """value (qword) -> [addresses holding it], across all of `segs`.

    Used to find, for a validated COL address, every place that points to
    it -- the vtable then starts 8 bytes after that pointer. Built directly
    off raw bytes rather than IDA xrefs, since none of this data has been
    typed as pointers yet.
    """
    index = {}
    for seg in segs:
        ea = (seg.start_ea + 7) & ~7
        while ea + 8 <= seg.end_ea:
            index.setdefault(ida_bytes.get_qword(ea), []).append(ea)
            ea += 8
    return index


def _find_cols(seg_start, seg_end, imagebase):
    """Every self-consistent RTTICompleteObjectLocator in [seg_start, seg_end)."""
    out = []
    ea = seg_start
    while ea + 24 <= seg_end:
        if ida_bytes.get_dword(ea) == 1:  # x64 COL signature
            offset   = ida_bytes.get_dword(ea + 4)
            cdoffset = ida_bytes.get_dword(ea + 8)
            td_rva   = ida_bytes.get_dword(ea + 12)
            chd_rva  = ida_bytes.get_dword(ea + 16)
            self_rva = ida_bytes.get_dword(ea + 20)
            # the load-independent cross-check: a COL always points at itself
            if imagebase + self_rva == ea and offset < 0x10000:
                out.append((ea, offset, cdoffset, imagebase + td_rva, imagebase + chd_rva))
        ea += 4
    return out


def _read_cstr(ea, maxlen=512):
    out = bytearray()
    for i in range(maxlen):
        b = ida_bytes.get_byte(ea + i)
        if b == 0:
            return bytes(out)
        out.append(b)
    return None  # unterminated within bound: reject rather than over-read


def _parse_type_descriptor(td_ea, imagebase, max_ea):
    """Validate + decode a candidate RTTITypeDescriptor. Returns (tag, tail)
    where tag is 'V' (class) or 'U' (struct) and tail is the mangled
    qualified name plus its trailing "@@", e.g. "CFoo@@" or "CFoo@NS@@"."""
    pvftable = ida_bytes.get_qword(td_ea)
    if not (imagebase <= pvftable < max_ea):
        return None
    name_field = _read_cstr(td_ea + 16)
    if name_field is None:
        return None
    try:
        text = name_field.decode("ascii")
    except UnicodeDecodeError:
        return None
    if len(text) < 5 or not text.startswith(".?A") or text[3] not in "VU" or not text.endswith("@@"):
        return None
    return text[3], text[4:]


def _friendly_name(tail):
    """"CFoo@NS@@" -> "NS::CFoo". Mangled qualifiers are innermost-first, so
    the split parts are reversed. Template arguments inside `tail` are left
    as-is -- this is a display fallback, not a demangler."""
    parts = [p for p in tail[:-2].split("@") if p]
    return "::".join(reversed(parts)) if parts else tail


def _sanitize(name):
    return _BAD_NAME_CHARS.sub("_", name)


def apply_rtti_names(json_path=JSON_PATH):
    if not _require_64bit():
        return False

    imagebase = _imagebase()
    max_ea = _max_ea()
    segs = _rdata_segments()
    if not segs:
        print("!! no .rdata-like segment found; nothing to scan")
        return False

    text_seg = ida_segment.get_segm_by_name(".text")
    code_start, code_end = (text_seg.start_ea, text_seg.end_ea) if text_seg else (imagebase, max_ea)

    ptr_index = _build_ptr_index(segs)

    n_col = n_valid_td = n_vtbl = n_named = 0
    classes = {}
    for seg in segs:
        for col_ea, offset, _cdoffset, td_ea, _chd_ea in _find_cols(seg.start_ea, seg.end_ea, imagebase):
            n_col += 1
            parsed = _parse_type_descriptor(td_ea, imagebase, max_ea)
            if not parsed:
                continue
            n_valid_td += 1
            tag, tail = parsed
            friendly = _friendly_name(tail)

            for ref_ea in ptr_index.get(col_ea, ()):
                vtbl_ea = ref_ea + 8
                target = ida_bytes.get_qword(vtbl_ea)
                if not (code_start <= target < code_end):
                    continue  # not actually a vtable slot 0 (a function pointer)
                n_vtbl += 1

                if offset == 0:
                    vtbl_name = "??_7" + tail + "6B@"
                    col_name = "??_R4" + tail + "6B@"
                else:
                    # multiple-inheritance secondary vtable: MSVC's real mangling
                    # also needs the base class name (from the class hierarchy
                    # descriptor, not walked here), so fall back to a friendly,
                    # non-mangled name rather than guess an incorrect one.
                    vtbl_name = _sanitize(f"RTTI_{friendly}_vtbl_{offset:#x}")
                    col_name = _sanitize(f"RTTI_{friendly}_col_{offset:#x}")
                td_name = "??_R0?A" + tag + tail + "@8"

                if ida_name.set_name(vtbl_ea, vtbl_name, ida_name.SN_NOWARN | ida_name.SN_FORCE):
                    n_named += 1
                ida_name.set_name(col_ea, col_name, ida_name.SN_NOWARN | ida_name.SN_FORCE)
                ida_name.set_name(td_ea, td_name, ida_name.SN_NOWARN | ida_name.SN_FORCE)

                cmt = f"RTTI: {friendly}"
                if offset != 0:
                    cmt += f" (secondary vtable, this-adjust {offset:#x})"
                idc.set_cmt(vtbl_ea, cmt, 0)

                classes[hex(vtbl_ea)] = {
                    "class": friendly,
                    "single_inheritance": offset == 0,
                    "offset": offset,
                    "type_descriptor": hex(td_ea),
                    "col": hex(col_ea),
                }

    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump({"imagebase": imagebase, "classes": classes}, fh, indent=1, ensure_ascii=False, sort_keys=True)

    print(f"RTTI: {n_col} COL candidate(s), {n_valid_td} with a valid TypeDescriptor, "
          f"{n_vtbl} vtable(s) located, {n_named} newly named -> {json_path}")
    return True


if __name__ == "__main__":
    apply_rtti_names()
