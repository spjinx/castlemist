# Moving the emu tooling off IDA, onto Ghidra

Research notes, 2026-10-08. Scope: `tools/emu/` (gw2patch + gw2emu) and the IDA
tooling it leans on for its addresses. Measured against the two installed
clients: `Gw2-64.exe` (PE timestamp 1790880995) and `Gw2-64-disable-aslr.exe`
(1789758548).

## Verdict

The emu is not tied to IDA by its code. Neither `gw2patch` nor `gw2emu` calls
IDA. It is tied to IDA by its **addresses**: every VA in `patches.h` and in the
research notes is an `sub_14…` name read off one IDB. That coupling has
already failed:

- `rc4-identity` expects `32 54 3B FF` at `0x140feb9ac`. Neither installed
  client has it there (`7d 10 00 0f` in the disable-ASLR copy, `0f b6 53 01` in
  the live one), so **gw2patch refuses to patch either build today**.
- The instruction still exists, once, in both builds, inside an identical
  36-byte context. It sits at `0x140fef30c` (disable-ASLR) and at
  `0x140ff446c` (live).

So the migration has two parts:

1. Stop needing a disassembler to *apply* patches. Byte signatures make
   gw2patch survive client updates by itself.
2. Use Ghidra for *finding* new things. That covers patch #3, patch #4, the
   pre-login message defs, and carrying the existing IDA knowledge forward.

## What depends on IDA today

| Piece | IDA dependence | Ghidra replacement |
| --- | --- | --- |
| `tools/emu/patcher/patches.h` | Hard VAs from the IDB; "re-anchor via IDA string xrefs" | Byte signatures in the patch table (no disassembler); a PyGhidra anchor script for new patches |
| `tools/emu/README.md` RE basis, `docs/research/gw2-net-crypto.md`, `gw2-login-flow.md` | `sub_` names and VAs of one IDB, already re-anchored once by hand (+0x2B20 drift) | Anchors stated as strings and signatures, with a script that prints the current VAs |
| Emu next steps (patch #3 call site, patch #4 GcSrv open producer, pre-login defs) | Hex-Rays pseudocode and xrefs | Ghidra decompiler and xrefs, after a full auto-analysis |
| `tools/gw2_annotations.json` + `ida_export/restore_annotations.py` | 8,694 functions (2,499 named), 6,406 data symbols, 38,807 comments, 1,018 folder entries, keyed by RVA + a 32-byte hash | A PyGhidra importer for the same JSON, plus Version Tracking for anything that moved |
| `tools/gw2_ida_symbols.json` + `ida_restore_symbols.py` | 324 functions, locals and labels, by absolute address | Same importer |
| `ida_apply_cmp_img_names.py` | Address-free (Perforce source-path strings) but written against IDAPython | Straight port: strings and xrefs are first-class in Ghidra's API |
| `ida_apply_rtti_names.py` | Custom RTTI walker | Mostly unnecessary: Ghidra's built-in Windows RTTI analyzer names vtables and classes during auto-analysis |
| `structs/gw2_ida_types*.h` | Plain C headers | Ghidra's C parser (File > Parse C Source); `#pragma pack(1)` is honoured |
| `structs/gen_gw2_json.py` | IDAPython | Already ported: `gen_gw2_json_ghidra.py`, verified again today (below) |
| `structs/dump_gw2_structs.py`, `templates/parseANStructs.idc` | IDAPython / IDC | Port alongside `gen_gw2_json_ghidra.py`; they share the reflection-table walk |
| Agent access ("py_exec_file over MCP") | An IDA MCP server | A Ghidra MCP server (see Tooling) |

## Measurements

### Headless PyGhidra works on this machine

Ghidra 12.1.3 + PyGhidra (`py -3.14`, JDK 21) ran
`gen_gw2_json_ghidra.py` headless against the live `Gw2-64.exe` with
`-noanalysis`. It took about a minute, wrote `fileTypes=31 chunks=69 types=6582`,
and recorded the new `source` block (`peTimestamp` 1790880995, which matches
the exe). A raw-bytes script needs no analysis. Decompiler and xref work
needs one full auto-analysis of the 43 MB image. Do it once and keep the
project (`~/gw2re.gpr` already exists and is a candidate).

### Patch sites by signature

| anchor | disable-ASLR | live | unique? |
| --- | --- | --- | --- |
| RC4 PRGA `xor dl,[rbx+rdi-1]` (`32 54 3B FF`) | `0x140fef30c` | `0x140ff446c` | yes, 1 hit each; 24 bytes of context identical across builds |
| bytes at the table's VA `0x140feb9ac` | `7d10000f` | `0fb65301` | (stale) |
| `"Msg::Raw::ClientRecvEncrypt"` | 1 | 1 | yes |
| `"MsgConnDispatch failed on message %u"` | 1 | 1 | yes |
| `"Auth failed to connect!"` / `"Connected to auth server %s"` | 1 | 1 | yes |
| `"NCPlatform.net"` | 1 | 1 | yes |
| `"ArenaNetworks.com"` | 3 | 3 | no: the string patch already rewrites every hit |

The signature
`0fb6540a08 4903d0 0fb6c2 0fb6540808 32543bff 8853ff` is the RC4 output step.
It is long enough to be unique and short enough to survive unrelated
recompiles. Scan `.text` for it, verify exactly one hit, and patch the
`32` → `8A`.

### How much of the IDA knowledge survives a patch

Of the 2,499 named functions in `gw2_annotations.json`:

- **15** still have their original bytes at their original RVA in the live
  client. That is the share an RVA-keyed restore (`ida_restore_annotations.py`)
  recovers today.
- **550** of the 1,678 that are 32 bytes or longer turn up **unchanged
  elsewhere** in `.text` (unique hash hit). A position-free re-anchor recovers
  those.
- 35 are ambiguous and 1,037 changed bytes. Most of the latter differ only in
  RIP-relative displacements that moved with the code. Those need a
  displacement-masked match, which is what Ghidra's Function ID, BSim and
  Version Tracking correlators do.

Also: the export's `local_types` holds 1,463 name-only lines and **no type
bodies** (zero `{`). The real type definitions exist only in the three
`gw2_ida_types*.h` headers. Import those; don't rely on the JSON for types.

## Plan

In order. Each step is useful alone.

1. **Done 2026-10-08. Signature patches in gw2patch.** Add an optional `signature` (with an
   offset to the patched byte) to `BytePatch`. Try the VA first, then fall
   back to a unique `.text` scan, and refuse when the hit count is not exactly
   one. This makes `rc4-identity` work on both installed builds again, with no
   disassembler at all.
2. **Done 2026-10-08. `tools/emu/ghidra/emu_anchors.py` (PyGhidra).** Six anchors
   resolve on both clients. In the disable-ASLR build, every auth-side function
   is exactly +0x1F0 from its July address and both MsgConn functions are
   exactly +0x3960. That is consistent with the code having moved rather than
   changed, so the old notes' descriptions still apply. Resolve each emu anchor
   from its string or signature to the containing function. Print a JSON of
   current VAs (ClientRecvEncrypt, the RC4 PRGA/KSA, the MsgConn dispatcher,
   DispatchStream, GcAuthCmdNotifyProc, the GcSrv command dispatcher). This is
   the "re-anchor via string xrefs" step, scripted. It runs headless after
   analysis.
3. **`ghidra_import_annotations.py`.** Read `gw2_annotations.json` and
   `gw2_ida_symbols.json` into a Ghidra program:
   - Names, comments and folders where the hash still matches at the RVA.
   - A hash search in `.text` for the rest.
   - A report of everything else.

   Then run **Version Tracking** from an old, annotated build to the current
   one for the 1,037 drifted functions.
4. **Port `ida_apply_cmp_img_names.py`** (string-anchored, so it ports
   directly). Then drop `ida_apply_rtti_names.py` in favour of Ghidra's RTTI
   analyzer, and keep the custom script only if the analyzer misses classes.
5. **Agent access.** Point `.mcp.json` at a Ghidra MCP server so the
   `py_exec_file` workflow has an equivalent.
6. **Then the emu RE itself, in Ghidra:**
   - Patch #3: the `sub_140FE5D00(...,2)` call site in the MsgConn wrapper
     ctor.
   - Patch #4: callers of the GcSrv command dispatcher with cmd == 2.
   - The pre-login message defs.

   Record each result as a string or signature anchor, not a bare VA.

## Tooling options

- **pyghidra-mcp** (clearbluejar): headless, stdio, pure pyghidra+JPype. The
  closest match to how `gen_gw2_json_ghidra.py` already runs.
- **Ghidra Headless MCP** (mrphrazer): headless, with patching, types and
  scripting exposed.
- **bethington/ghidra-mcp**: a GUI plugin plus a headless server, with broad
  write access (rename, retype, comment).
- IDA → Ghidra in one shot: Ghidra ships `GhidraBuild/IDAPro` (an `xml_exporter`
  for IDA and the matching XML importer). It needs a working IDA with
  IDAPython, so it only applies while an IDA Pro install is still around. IDA
  Free has no IDAPython, which is the same reason `gen_gw2_json_ghidra.py`
  exists.

Sources:
[pyghidra-mcp](https://github.com/clearbluejar/pyghidra-mcp) ·
[Ghidra Headless MCP](https://vibeindex.ai/mcp/mrphrazer/ghidra-headless-mcp) ·
[bethington/ghidra-mcp](https://docsearch.algolia.com/mcp/docs/repo/bethington/ghidra-mcp) ·
[Ghidra's IDAPro export scripts](https://git.sudo.is/mirrors/ghidra/src/branch/master/GhidraBuild/IDAPro) ·
[GhIDA / idaxml](https://awesome.ecosyste.ms/projects/github.com%2Fcisco-talos%2Fghida)
