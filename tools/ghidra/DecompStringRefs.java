// For every ASCII string containing one of the given substrings, find RIP-relative LEA
// instructions that load it (no auto-analysis needed) and decompile each containing
// function. Args: outFile maxFunctionsPerString substr1 [substr2 ...]. Read-only.
// @category castlemist
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.io.PrintWriter;
import java.util.*;

public class DecompStringRefs extends GhidraScript {
    private final List<MemoryBlock> code = new ArrayList<>();
    private final List<byte[]> codeBytes = new ArrayList<>();

    private List<Address> leaRefs(Address target) {
        List<Address> hits = new ArrayList<>();
        long t = target.getOffset();
        for (int k = 0; k < code.size(); k++) {
            byte[] c = codeBytes.get(k);
            long base = code.get(k).getStart().getOffset();
            for (int j = 0; j + 7 <= c.length; j++) {
                int rex = c[j] & 0xff;
                if ((rex != 0x48 && rex != 0x4c) || (c[j + 1] & 0xff) != 0x8d || (c[j + 2] & 0xc7) != 0x05) continue;
                int disp = (c[j + 3] & 0xff) | (c[j + 4] & 0xff) << 8 | (c[j + 5] & 0xff) << 16 | (c[j + 6] << 24);
                if (base + j + 7 + disp == t) hits.add(code.get(k).getStart().add(j));
            }
        }
        return hits;
    }

    // No function covers `r` (the program was never auto-analyzed): walk back to the
    // int3 padding MSVC puts between functions, create one there (in memory only; the
    // project is opened read-only) and return it if it covers `r`.
    private Function makeFunction(Address r) throws Exception {
        Memory mem = currentProgram.getMemory();
        Address a = r;
        for (int back = 0; back < 0x20000; back++) {
            Address p = a.subtract(1);
            if ((mem.getByte(p) & 0xff) == 0xcc && (a.getOffset() & 0xf) == 0) break;
            a = p;
        }
        Function f = getFunctionAt(a);
        if (f != null && !f.getBody().contains(r)) {  // a stub with an incomplete body: redo it
            removeFunction(f);
            f = null;
        }
        if (f == null) {
            boolean dis = disassemble(a);
            f = createFunction(a, null);
            println("MAKEFN at " + a + " for " + r + " disassembled=" + dis + " created=" + (f != null)
                    + (f != null ? " body=" + f.getBody().getMinAddress() + ".." + f.getBody().getMaxAddress() : ""));
        }
        return getFunctionContaining(r);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        int maxPer = Integer.parseInt(args[1]);
        Memory mem = currentProgram.getMemory();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || !b.isExecute()) continue;
            byte[] c = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), c);
            code.add(b);
            codeBytes.add(c);
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Address> done = new HashSet<>();
        for (int i = 2; i < args.length; i++) {
            byte[] needle = args[i].getBytes("US-ASCII");
            for (MemoryBlock b : mem.getBlocks()) {
                if (!b.isInitialized() || b.isExecute()) continue;
                Address a = b.getStart();
                while (a != null && a.compareTo(b.getEnd()) < 0) {
                    Address hit = mem.findBytes(a, b.getEnd(), needle, null, true, monitor);
                    if (hit == null) break;
                    Address s = hit;
                    while (s.compareTo(b.getStart()) > 0) {
                        int ch = mem.getByte(s.subtract(1)) & 0xff;
                        if (ch < 0x20 || ch >= 0x7f) break;
                        s = s.subtract(1);
                    }
                    // source paths begin "D:\" -- skip any printable junk before that
                    for (Address q = s; q.compareTo(hit) < 0; q = q.add(1))
                        if (mem.getByte(q) == 'D' && mem.getByte(q.add(1)) == ':') { s = q; break; }
                    List<Address> refs = leaRefs(s);
                    out.println("### STRING " + s + " '" + args[i] + "' lea refs=" + refs.size());
                    int n = 0;
                    for (Address r : refs) {
                        Function f = getFunctionContaining(r);
                        if (f == null) f = makeFunction(r);
                        out.println("  ref " + r + " in " + (f == null ? "(none)" : f.getName() + " @" + f.getEntryPoint()));
                        if (f == null || !done.add(f.getEntryPoint()) || n++ >= maxPer) continue;
                        DecompileResults res = dec.decompileFunction(f, 120, monitor);
                        if (res != null && res.decompileCompleted())
                            out.println("--- " + f.getName() + " @" + f.getEntryPoint() + "\n" + res.getDecompiledFunction().getC());
                    }
                    a = hit.add(needle.length);
                }
            }
        }
        out.close();
        println("DONE functions=" + done.size());
    }
}
