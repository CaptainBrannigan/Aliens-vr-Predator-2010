// Applies names + comments from C:\Dev\VR Mod\AVP2010\main\docs\re\avp_symbols.csv
// (address,kind,name,area,confidence,role). kind F = function, L = label.
// Only renames default-named functions (FUN_xxx) or ones already carrying our name,
// so names you set by hand in the GUI are never overwritten. Must run WITHOUT -readOnly.
// @category AvP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ApplySymbols extends GhidraScript {
    private static List<String> parseCsvLine(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean q = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (c == '"') q = !q;
            else if (c == ',' && !q) { out.add(cur.toString()); cur.setLength(0); }
            else cur.append(c);
        }
        out.add(cur.toString());
        return out;
    }

    @Override
    public void run() throws Exception {
        File csv = new File("C:\\Dev\\VR Mod\\AVP2010\\main\\docs\\re\\avp_symbols.csv");
        BufferedReader br = new BufferedReader(new FileReader(csv));
        String line = br.readLine(); // header
        Listing listing = currentProgram.getListing();
        SymbolTable st = currentProgram.getSymbolTable();
        int renamed = 0, labeled = 0, skipped = 0;
        while ((line = br.readLine()) != null) {
            if (line.trim().isEmpty()) continue;
            List<String> f = parseCsvLine(line);
            Address a = toAddr(f.get(0));
            String kind = f.get(1), name = f.get(2), conf = f.get(4), role = f.get(5);
            String comment = "[AvP " + f.get(3) + ", " + conf + "] " + role;
            if (kind.equals("F")) {
                Function fn = getFunctionAt(a);
                if (fn == null && listing.getFunctionContaining(a) == null) {
                    disassemble(a);
                    fn = createFunction(a, null);
                }
                if (fn == null) {
                    println("SKIP (not a function entry): " + a + " " + name);
                    skipped++;
                    continue;
                }
                String cur = fn.getName();
                if (cur.startsWith("FUN_") || cur.equals(name)) {
                    fn.setName(name, SourceType.USER_DEFINED);
                    renamed++;
                } else {
                    println("KEEP existing name " + cur + " at " + a + " (wanted " + name + ")");
                }
                fn.setComment(comment);
            } else {
                Symbol s = st.getPrimarySymbol(a);
                if (s == null || s.getSource() == SourceType.DEFAULT || s.getSource() == SourceType.ANALYSIS) {
                    Symbol ns = st.createLabel(a, name, SourceType.USER_DEFINED);
                    ns.setPrimary();
                    labeled++;
                }
                listing.setComment(a, CodeUnit.PLATE_COMMENT, comment);
            }
        }
        br.close();
        println("ApplySymbols: renamed " + renamed + " functions, labeled " + labeled + ", skipped " + skipped);
    }
}
