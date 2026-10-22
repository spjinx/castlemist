# Ghidra research scripts

Headless helpers for reading `Gw2-64.exe` statically. Both work on a program that was
imported without auto-analysis: they find code by byte scan, create any missing
function in memory, and decompile it. Run them with `-readOnly` so nothing is saved.

| script | args | finds |
|---|---|---|
| `DecompStringRefs.java` | `outFile maxPerString substr...` | functions that load a string (an assert's source path, e.g. `EfCliMsg.cpp`) with a rip-relative LEA |
| `DecompBytes.java` | `outFile maxFunctions hex` | functions containing a byte pattern (e.g. `bab7000000` = `mov edx, 0xB7`, a content lookup of type 183); appends the text of every `UNK_` string they use |

```
set JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-21.0.1.12-hotspot
support\analyzeHeadless.bat C:\Users\vital gw2re -process Gw2-64.exe -noanalysis -readOnly ^
    -scriptPath <castlemist>\tools\ghidra -postScript DecompBytes.java out.c 60 bab7000000
```

Function starts come from the int3 padding before them. `tools/emu/ghidra/emu_anchors.py`
uses the exception directory instead, which is more exact; port to that if a boundary is
ever wrong. Findings so far: docs/research/gw2-skill-effects.md.
