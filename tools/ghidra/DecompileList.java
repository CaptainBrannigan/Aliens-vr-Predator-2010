// Decompiles a list of addresses (script args: <outname> <addr> [addr...]) to
// C:\Dev\VR Mod\AVP2010\analysis\<outname>.txt, with callers and callees for each.
// @category AvP
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { printerr("usage: <outname> <addr>..."); return; }
        File out = new File("C:\\Dev\\VR Mod\\AVP2010\\analysis\\" + args[0] + ".txt");
        out.getParentFile().mkdirs();
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();

        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(args[i]);
            Function f = listing.getFunctionContaining(a);
            if (f == null) f = getFunctionAt(a);
            if (f == null) {
                disassemble(a);
                f = createFunction(a, null);
            }
            pw.println("==================================================================");
            if (f == null) { pw.println("NO FUNCTION at " + a); continue; }
            pw.println(f.getName() + " @ " + f.getEntryPoint() + "  size=" + f.getBody().getNumAddresses());

            Set<String> callers = new TreeSet<>();
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                Function c = listing.getFunctionContaining(r.getFromAddress());
                callers.add((c == null ? "?" : c.getName()) + "@" + r.getFromAddress() + "(" + r.getReferenceType() + ")");
            }
            pw.println("callers (" + callers.size() + "): " + String.join(", ", callers));
            Set<String> callees = new TreeSet<>();
            for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getName());
            pw.println("callees (" + callees.size() + "): " + String.join(", ", callees));

            DecompileResults res = di.decompileFunction(f, 120, monitor);
            if (res != null && res.decompileCompleted())
                pw.println(res.getDecompiledFunction().getC());
            else
                pw.println("DECOMPILE FAILED: " + (res == null ? "null" : res.getErrorMessage()));
        }
        di.dispose();
        pw.close();
        println("Wrote " + out);
    }
}
