// Finds defined strings containing any keyword (case-insensitive) and lists the
// functions that reference each one. Output: <project dir>\..\analysis\keyword_xrefs.txt
// Keywords can be overridden with script args: -postScript KeywordStringXrefs.java Camera Aim
// @category AvP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class KeywordStringXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] kws = getScriptArgs();
        String outName = "keyword_xrefs";
        // "out:" form, because analyzeHeadless.bat (cmd) splits arguments on '='
        if (kws.length > 0 && (kws[0].startsWith("out=") || kws[0].startsWith("out:"))) {
            outName = kws[0].substring(4);
            kws = java.util.Arrays.copyOfRange(kws, 1, kws.length);
        }
        if (kws.length == 0) kws = new String[] { "Controller", "Aim", "Player", "Camera" };
        File out = new File("C:\\Dev\\VR Mod\\AVP2010\\analysis\\" + outName + ".txt");
        out.getParentFile().mkdirs();
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Map<String, Set<String>> funcsPerKw = new TreeMap<>();
        Map<String, Integer> funcHitCount = new HashMap<>();

        DataIterator it = listing.getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (!d.hasStringValue()) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            String lower = s.toLowerCase();
            List<String> matched = new ArrayList<>();
            for (String k : kws) if (lower.contains(k.toLowerCase())) matched.add(k);
            if (matched.isEmpty()) continue;

            Address a = d.getAddress();
            StringBuilder refs = new StringBuilder();
            int nrefs = 0;
            for (Reference r : rm.getReferencesTo(a)) {
                nrefs++;
                Address from = r.getFromAddress();
                Function f = listing.getFunctionContaining(from);
                String fn = f == null ? ("<nofunc " + from + ">") : (f.getName() + "@" + f.getEntryPoint());
                refs.append("\n      ").append(from).append("  in ").append(fn);
                if (f != null) {
                    String key = f.getName() + "@" + f.getEntryPoint();
                    funcHitCount.merge(key, 1, Integer::sum);
                    for (String k : matched) funcsPerKw.computeIfAbsent(k, x -> new TreeSet<>()).add(key);
                }
            }
            String shown = s.length() > 160 ? s.substring(0, 160) + "..." : s;
            pw.println(a + "  [" + String.join(",", matched) + "] refs=" + nrefs + "  \"" + shown.replace("\n", "\\n") + "\"" + refs);
        }

        pw.println("\n===== Functions per keyword =====");
        for (Map.Entry<String, Set<String>> e : funcsPerKw.entrySet())
            pw.println(e.getKey() + ": " + e.getValue().size() + " functions");
        pw.println("\n===== Functions referencing the most keyword strings =====");
        List<Map.Entry<String, Integer>> top = new ArrayList<>(funcHitCount.entrySet());
        top.sort((x, y) -> y.getValue() - x.getValue());
        for (int i = 0; i < Math.min(80, top.size()); i++)
            pw.println(String.format("%4d  %s", top.get(i).getValue(), top.get(i).getKey()));
        pw.close();
        println("Wrote " + out);
    }
}
