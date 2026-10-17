// Script args: <outname> then any of
//   x:ADDR      list every reference to ADDR with the containing function and ref type
//   v:ADDR:N    dump N pointer entries of a vtable at ADDR with target function names
// Output: C:\Dev\VR Mod\AVP2010\analysis\<outname>.txt
// @category AvP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class XrefsAndVtables extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File("C:\\Dev\\VR Mod\\AVP2010\\analysis\\" + args[0] + ".txt");
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (int i = 1; i < args.length; i++) {
            String[] p = args[i].split(":");
            Address a = toAddr(p[1]);
            if (p[0].equals("x")) {
                pw.println("=== xrefs to " + a);
                for (Reference r : rm.getReferencesTo(a)) {
                    Function f = listing.getFunctionContaining(r.getFromAddress());
                    Instruction ins = listing.getInstructionAt(r.getFromAddress());
                    pw.println("  " + r.getFromAddress() + "  " + r.getReferenceType() + "  in "
                        + (f == null ? "?" : f.getName()) + "   " + (ins == null ? "" : ins.toString()));
                }
            } else if (p[0].equals("v")) {
                int n = Integer.parseInt(p[2]);
                pw.println("=== vtable " + a);
                for (int k = 0; k < n; k++) {
                    Address slot = a.add(k * 4);
                    Address tgt = toAddr(getInt(slot) & 0xffffffffL);
                    Function f = getFunctionAt(tgt);
                    pw.println(String.format("  [%2d] +0x%02X  %s  %s", k, k * 4, tgt,
                        f == null ? "(no function)" : f.getName() + " size=" + f.getBody().getNumAddresses()));
                }
            }
        }
        pw.close();
        println("Wrote " + out);
    }
}
