// Lists external (imported) functions whose library or name matches any script arg
// (case-insensitive), with every function that references them.
// Script args: <outname> <filter>... Output: C:\Dev\VR Mod\AVP2010\analysis\<outname>.txt
// @category AvP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ImportCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File("C:\\Dev\\VR Mod\\AVP2010\\analysis\\" + args[0] + ".txt");
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();
        for (Function ext : fm.getExternalFunctions()) {
            ExternalLocation loc = ext.getExternalLocation();
            String lib = loc.getLibraryName();
            String full = (lib + "::" + ext.getName()).toLowerCase();
            boolean match = false;
            for (int i = 1; i < args.length; i++) if (full.contains(args[i].toLowerCase())) match = true;
            if (!match) continue;
            pw.println("=== " + lib + "::" + ext.getName());
            Set<Address> seen = new HashSet<>();
            List<Address> targets = new ArrayList<>();
            targets.add(ext.getEntryPoint());
            // IAT slots / thunks referencing the external symbol
            for (Reference r : rm.getReferencesTo(ext.getEntryPoint())) targets.add(r.getFromAddress());
            for (Function th : fm.getFunctions(true)) {
                if (th.isThunk() && th.getThunkedFunction(true) == ext) targets.add(th.getEntryPoint());
            }
            for (Address t : targets) {
                for (Reference r : rm.getReferencesTo(t)) {
                    Address from = r.getFromAddress();
                    if (!seen.add(from)) continue;
                    Function f = listing.getFunctionContaining(from);
                    if (f == null && !from.isMemoryAddress()) continue;
                    pw.println("  " + from + "  " + r.getReferenceType() + "  in " + (f == null ? "?" : f.getName()));
                }
            }
        }
        pw.close();
        println("Wrote " + out);
    }
}
