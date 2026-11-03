// Find where an address is referenced as data: 8-byte pointers to it anywhere in initialized
// memory (vtables, tables), then the code that loads each such table with a rip-relative LEA,
// and decompile those functions. Args: outFile target1 [target2 ...] (hex). Read-only project.
// @category castlemist
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindPointerRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int t = 1; t < args.length; t++) {
            long target = Long.parseLong(args[t], 16);
            // 1. 8-byte pointers to the target.
            Set<Long> ptrs = new LinkedHashSet<>();
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isInitialized() || b.getSize() > 0x7fffffffL) continue;
                byte[] buf = new byte[(int) b.getSize()];
                b.getBytes(b.getStart(), buf);
                long base = b.getStart().getOffset();
                for (int i = 0; i + 8 <= buf.length; i += 8) {
                    long v = 0;
                    for (int k = 7; k >= 0; k--) v = (v << 8) | (buf[i + k] & 0xff);
                    if (v == target) ptrs.add(base + i);
                }
            }
            out.println("### TARGET " + Long.toHexString(target) + " pointers=" + ptrs.size());
            // 2. For each pointer, walk back to the table start (previous non-code-pointer) and find LEAs to
            //    any slot from the pointer down to 32 slots before it.
            for (long p : ptrs) {
                out.println("  pointer at " + Long.toHexString(p));
                Set<Long> leaFuncs = new LinkedHashSet<>();
                for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                    if (!b.isExecute() || !b.isInitialized()) continue;
                    byte[] buf = new byte[(int) b.getSize()];
                    b.getBytes(b.getStart(), buf);
                    long base = b.getStart().getOffset();
                    for (int i = 0; i + 7 <= buf.length; i++) {
                        // 48/4C 8D modrm(rip) disp32
                        if ((buf[i] & 0xfb) != 0x48 || (buf[i + 1] & 0xff) != 0x8d || (buf[i + 2] & 0xc7) != 0x05) continue;
                        int disp = (buf[i + 3] & 0xff) | ((buf[i + 4] & 0xff) << 8) | ((buf[i + 5] & 0xff) << 16) | ((buf[i + 6] & 0xff) << 24);
                        long tgt = base + i + 7 + disp;
                        if (tgt > p || tgt < p - 32 * 8) continue;
                        out.println("    lea " + Long.toHexString(tgt) + " (slot " + ((p - tgt) / 8) + ") at " + Long.toHexString(base + i));
                        Address a = toAddr(base + i);
                        Function f = getFunctionContaining(a);
                        if (f == null) {
                            long s = base + i;
                            while (s > base && (buf[(int) (s - base - 1)] & 0xff) != 0xcc) s--;
                            f = createFunction(toAddr(s), null);
                        }
                        if (f != null) leaFuncs.add(f.getEntryPoint().getOffset());
                    }
                }
                for (long fe : leaFuncs) {
                    Function f = getFunctionAt(toAddr(fe));
                    DecompileResults r = di.decompileFunction(f, 60, monitor);
                    out.println("--- " + f.getName() + " @" + Long.toHexString(fe));
                    out.println(r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "(no decompile)");
                }
            }
        }
        out.close();
    }
}
