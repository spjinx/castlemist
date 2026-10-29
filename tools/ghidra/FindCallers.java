// Find direct callers (E8 rel32 calls) of the given functions by scanning executable memory,
// create each calling function in memory if missing (the project is read-only), and decompile it.
// Args: outFile target1 [target2 ...] (hex, e.g. 140ac9e90).
// @category castlemist
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

public class FindCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int t = 1; t < args.length; t++) {
            long target = Long.parseLong(args[t], 16);
            Map<Long, Address> callers = new LinkedHashMap<>();
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isExecute() || !b.isInitialized()) continue;
                byte[] buf = new byte[(int) b.getSize()];
                b.getBytes(b.getStart(), buf);
                long base = b.getStart().getOffset();
                for (int i = 0; i + 5 <= buf.length; i++) {
                    if ((buf[i] & 0xff) != 0xE8) continue;
                    int rel = (buf[i + 1] & 0xff) | ((buf[i + 2] & 0xff) << 8) | ((buf[i + 3] & 0xff) << 16) | (buf[i + 4] << 24);
                    if (base + i + 5 + rel == target) callers.put(base + i, toAddr(base + i));
                }
            }
            out.printf("### TARGET %x callers=%d%n", target, callers.size());
            for (Address site : callers.values()) {
                Function f = getFunctionContaining(site);
                if (f == null) {
                    disassemble(site);
                    // walk back to the int3/nop padding that precedes a function start
                    Address a = site;
                    for (int k = 0; k < 0x20000; k++) {
                        Address p = a.subtract(1);
                        int v = getByte(p) & 0xff;
                        if (v == 0xCC) break;
                        a = p;
                    }
                    disassemble(a);
                    f = createFunction(a, null);
                    if (f == null) f = getFunctionContaining(site);
                }
                out.printf("  call %s in %s%n", site, f == null ? "?" : f.getName() + " @" + f.getEntryPoint());
                if (f == null) continue;
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                if (r.decompileCompleted()) {
                    out.printf("--- %s @%s%n%s%n", f.getName(), f.getEntryPoint(), r.getDecompiledFunction().getC());
                }
            }
        }
        out.close();
    }
}
