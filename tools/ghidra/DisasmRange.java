// Disassembly listing. Script args: <outname> <addr>:<count> [...]
// Output: C:\Dev\VR Mod\AVP2010\analysis\<outname>.txt
// @category AvP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.*;

public class DisasmRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File("C:\\Dev\\VR Mod\\AVP2010\\analysis\\" + args[0] + ".txt");
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        Listing listing = currentProgram.getListing();
        for (int i = 1; i < args.length; i++) {
            String[] p = args[i].split(":");
            Address a = toAddr(p[0]);
            int n = Integer.parseInt(p[1]);
            pw.println("=== " + a);
            Instruction ins = listing.getInstructionContaining(a);
            for (int k = 0; k < n && ins != null; k++) {
                pw.println("  " + ins.getAddress() + "  " + ins);
                ins = ins.getNext();
            }
        }
        pw.close();
        println("Wrote " + out);
    }
}
