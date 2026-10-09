# gw2emu — client-only GW2 server emulator (scaffold)

Goal: run the GW2 client against a **local emulator** with no connection to the
real ArenaNet servers (no live traffic → nothing to ban). Approach chosen:
**patch the crypto out of the client** so the AuthSrv MsgConn channel is
plaintext, then speak that plaintext protocol from a small static-linked C++
server. Milestone ladder: reach the **login screen** offline first, then grow
message handling from there.

This is a scaffold: the transport + framing are real and match the
reverse-engineering; the server-authoritative game logic is intentionally a
set of stubs to grow into.

> **Addresses below are from the July 2026 IDB.** Client patches move them.
> gw2patch no longer depends on them: each patch carries a byte signature,
> and `ghidra/emu_anchors.py` re-finds the functions in any build (see
> "Finding things in a new build"). For the measurements behind that, see
> [docs/research/emu-ghidra-migration.md](../../docs/research/emu-ghidra-migration.md).

## Reverse-engineering basis (Gw2-64-disable-aslr.exe, imagebase 0x140000000)

Two stacked layers, established by RE:

| Layer | Detail |
|---|---|
| Socket TLS/RSA + pinned **StartCom** cert | The **NCPlatform HTTPS portal**; issues a 20-byte session key. *Not* on the AuthSrv message path. |
| MsgConn RC4 | AuthSrv transport. `seed = PRF(sessionKey20, bakedConst @ 0x1421117C0)`; textbook RC4 (`sub_140FEB950`); mode `conn+264` 2→3. |

Receive dispatcher `sub_140FE6FE0`: mode 1 = plaintext stream, mode 2 =
raw `[type][len]` connect, mode 3 = RC4 → `DispatchStream` (`sub_140FE62B0`).
Wire message = `[msgId u16 LE][fields per def]`, no length prefix.

## Patches (`gw2patch`)

| # | Patch | Status |
|---|---|---|
| 2 | **RC4 → identity** — `32→8A` (`xor dl,[..]`→`mov dl,[..]`). Whole channel becomes plaintext. Found by signature; `0x140fef30c` (disable-ASLR) / `0x140ff446c` (live) on the 2026-10 clients | ✅ concrete, signature-anchored |
| 1 | Endpoint strings → `127.0.0.1` (`ArenaNetworks.com`, `NCPlatform.net`) | ⚠ best-effort string replace |
| 3 | Force MsgConn mode 1 (alt to #2) | 🔧 documented, needs call-site disasm |
| 4 | Portal→AuthSrv open trigger (run fully offline w/o portal) | ⛔ TODO — next RE step |

See `patcher/patches.h`. Each byte patch is located in this order:

1. The recorded VA, if the expected bytes are there (same build as the table).
2. Otherwise the patch's **signature**, scanned across the executable sections.
   It must match exactly once, or nothing is written.
3. If the patched form of the signature is found instead, the patch is
   reported as already applied (`[=]`), not as a failure.

Only patch **a copy** of the client:

```
bin\gw2patch.exe "Gw2-64-disable-aslr.exe" "Gw2-emu.exe"
```

## Emulator (`gw2emu`)

- Listens on `127.0.0.1:<port>` (default 6112), plaintext MsgConn.
- On connect: sends `[00][16][20×00]` to push the client mode 2→3.
- Splits the stream into messages using the registered `MsgDef`s
  (`src/emu/protocol.cpp`) and logs them; unknown ids are hex-dumped.
- `src/emu/msgconn.cpp` implements the def-driven field codec matching the
  client's `MsgUnpack` type codes (`src/emu/msgdef.h`).

```
bin\gw2emu.exe 6112
```

## Finding things in a new build

`ghidra/emu_anchors.py` resolves the functions these notes talk about from
strings and signatures that survive a client patch. It maps each hit to its
function through the PE exception table (`.pdata`), so it needs no
auto-analysis.

| name it gives | anchored on |
| --- | --- |
| `emu_Msg_Raw_ClientRecvEncrypt` | `"Msg::Raw::ClientRecvEncrypt"` |
| `emu_MsgConnDispatch` | `"MsgConnDispatch failed on message %u: %s."` |
| `emu_Gc_GcAuthCmdNotifyProc` | `"Auth failed to connect!"`, checked against two more of its strings |
| `emu_AccountLoginResult` | `"Account login transaction result %u"` |
| `emu_PortalUserInfoError` | `"Error retriving user info from Portal: ..."` |
| `emu_Rc4Prga` + label `emu_rc4_identity_site` | the rc4-identity signature |

In Ghidra (started with `support\pyghidraRun.bat`), add `tools/emu/ghidra/` in
the Script Manager and run it with the client open. It names the functions,
adds a plate comment saying how each was found, and writes
`bin/emu_anchors.json`. Without Ghidra, for a quick look:

```
python tools/emu/ghidra/emu_anchors.py "<...>\Gw2-64.exe"
```

To anchor something new, add a unique string or signature to the tables at the
top of the script. A string the function prints is the most durable choice.

## Build

MinGW-w64 `g++` on PATH, then:

```
build.bat
```

Produces static binaries in `bin\` (`-static -static-libgcc -static-libstdc++`,
`-lws2_32`) — no runtime DLL deps.

## Next steps

1. **Patch #4**: find the producer of the GcSrv "open AuthSrv" command
   (callers into the GcSrv command dispatcher, `sub_14023F920` in the July IDB, case 2) to trigger the connection offline with
   a loopback address + dummy key — closes the last gap to a login screen.
2. **Capture pre-login defs**: instrument `DispatchStream` on a real auth login
   to record the server→client message defs, add them to `protocol.cpp`, and
   craft responses until the Coherent login HTML renders.
3. Grow handlers upward (character select → world enter).
