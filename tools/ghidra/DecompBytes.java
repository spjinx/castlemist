// Decompile every function containing a byte pattern in executable memory (creating
// missing functions in memory; the project is read-only). Args: outFile maxFunctions hex
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

public class DecompBytes extends GhidraScript {
    private Function functionFor(Address r) throws Exception {
        Function f = getFunctionContaining(r);
        if (f != null) return f;
        Memory mem = currentProgram.getMemory();
        Address a = r;
        for (int back = 0; back < 0x20000; back++) {
            Address p = a.subtract(1);
            if ((mem.getByte(p) & 0xff) == 0xcc && (a.getOffset() & 0xf) == 0) break;
            a = p;
        }
        f = getFunctionAt(a);
        if (f != null && !f.getBody().contains(r)) { removeFunction(f); f = null; }
        if (f == null) { disassemble(a); createFunction(a, null); }
        return getFunctionContaining(r);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        int max = Integer.parseInt(args[1]);
        String h = args[2];
        byte[] needle = new byte[h.length() / 2];
        for (int i = 0; i < needle.length; i++) needle[i] = (byte) Integer.parseInt(h.substring(2 * i, 2 * i + 2), 16);
        Memory mem = currentProgram.getMemory();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Address> done = new HashSet<>();
        int hits = 0;
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || !b.isExecute()) continue;
            Address a = b.getStart();
            while (a != null && a.compareTo(b.getEnd()) < 0) {
                Address hit = mem.findBytes(a, b.getEnd(), needle, null, true, monitor);
                if (hit == null) break;
                hits++;
                Function f = functionFor(hit);
                out.println("HIT " + hit + " in " + (f == null ? "(none)" : f.getName() + " @" + f.getEntryPoint()));
                if (f != null && done.size() < max && done.add(f.getEntryPoint())) {
                    DecompileResults res = dec.decompileFunction(f, 120, monitor);
                    if (res != null && res.decompileCompleted()) {
                        String c = res.getDecompiledFunction().getC();
                        out.println("--- " + f.getName() + " @" + f.getEntryPoint() + "\n" + c);
                        java.util.regex.Matcher m = java.util.regex.Pattern.compile("UNK_([0-9a-f]{9})").matcher(c);
                        Set<String> seen = new TreeSet<>();
                        while (m.find()) seen.add(m.group(1));
                        for (String s : seen) {
                            Address sa = toAddr(Long.parseLong(s, 16));
                            StringBuilder sb = new StringBuilder();
                            for (int k = 0; k < 200; k++) {
                                int ch = mem.getByte(sa.add(k)) & 0xff;
                                if (ch < 0x20 || ch >= 0x7f) break;
                                sb.append((char) ch);
                            }
                            if (sb.length() >= 4) out.println("  // UNK_" + s + " = \"" + sb + "\"");
                        }
                    }
                }
                a = hit.add(1);
            }
        }
        out.close();
        println("DONE hits=" + hits + " functions=" + done.size());
    }
}
