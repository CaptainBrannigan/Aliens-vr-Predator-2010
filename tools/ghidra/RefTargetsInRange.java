// Lists every address in [start,end) that is the destination of a direct reference,
// with ref count and one sample referencing function. Script args: <outname> <start> <end>
// Useful to find where one static array ends and the next global begins.
// @category AvP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class RefTargetsInRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File("C:\\Dev\\VR Mod\\AVP2010\\analysis\\" + args[0] + ".txt");
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        Address start = toAddr(args[1]), end = toAddr(args[2]);
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();
        AddressIterator it = rm.getReferenceDestinationIterator(new AddressSet(start, end.subtract(1)), true);
        while (it.hasNext()) {
            Address a = it.next();
            int n = 0;
            String sample = "";
            for (Reference r : rm.getReferencesTo(a)) {
                if (n == 0) {
                    Function f = listing.getFunctionContaining(r.getFromAddress());
                    sample = (f == null ? "?" : f.getName()) + "@" + r.getFromAddress();
                }
                n++;
            }
            pw.println(String.format("%s  +0x%X  refs=%d  e.g. %s", a, a.subtract(start), n, sample));
        }
        pw.close();
        println("Wrote " + out);
    }
}
