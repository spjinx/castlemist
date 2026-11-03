// Dump memory of the project's program (read-only). Args: outFile kind addr count
// kind: q (qwords, with the function each points into), f (floats), d (dwords).
// @category castlemist
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class ReadMem extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        String kind = args[1];
        Address a = toAddr(Long.parseLong(args[2], 16));
        int n = Integer.parseInt(args[3]);
        for (int i = 0; i < n; i++) {
            if (kind.equals("q")) {
                Address at = a.add(8L * i);
                long v = getLong(at);
                Function f = getFunctionContaining(toAddr(v));
                out.printf("%s %x %s%n", at, v, f == null ? "-" : f.getName());
            } else if (kind.equals("f")) {
                Address at = a.add(4L * i);
                out.printf("%s %s %08x%n", at, Float.toString(getFloat(at)), getInt(at));
            } else {
                Address at = a.add(4L * i);
                out.printf("%s %08x%n", at, getInt(at));
            }
        }
        out.close();
    }
}
