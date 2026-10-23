// Decompile the functions at the given addresses (creating any missing one in memory; the
// project is read-only). Args: outFile addr1 [addr2 ...] (hex, e.g. 140e1f0c0).
// @category castlemist
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DecompAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(Long.parseLong(args[i], 16));
            Function f = getFunctionAt(a);
            if (f == null) {
                disassemble(a);
                f = createFunction(a, null);
            }
            if (f == null) { out.println("### no function at " + a); continue; }
            DecompileResults res = dec.decompileFunction(f, 120, monitor);
            out.println("--- " + f.getName() + " @" + a);
            out.println(res != null && res.decompileCompleted() ? res.getDecompiledFunction().getC() : "(decompile failed)");
        }
        out.close();
        println("DONE");
    }
}
