// Lists every CALL through a memory operand [reg + disp] for the given displacements (virtual calls by vtable slot),
// with the containing function. Script args: <outname> <disp> [disp...] (hex, e.g. 0xb8)
// Output: C:\Dev\VR Mod\AVP2010\analysis\<outname>.txt
// @category AvP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class IndirectCalls extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File("C:\\Dev\\VR Mod\\AVP2010\\analysis\\" + args[0] + ".txt");
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        List<Long> disps = new ArrayList<>();
        for (int i = 1; i < args.length; ++i)
            disps.add(Long.decode(args[i]));
        Map<Long, List<String>> hits = new TreeMap<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int calls = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            // MSVC's usual form: MOV reg2, [reg1 + disp] then CALL reg2 within 24 instructions (arguments pushed between).
            if (ins.getMnemonicString().equalsIgnoreCase("MOV") && ins.getNumOperands() == 2 &&
                ins.toString().contains("[")) {
                Object[] dst = ins.getOpObjects(0);
                Object[] src = ins.getOpObjects(1);
                if (dst.length == 1 && dst[0] instanceof Register && !ins.toString().split(",")[0].contains("[")) {
                    Long d = null;
                    int sc = 0, regs = 0;
                    for (Object o : src) {
                        if (o instanceof Scalar) {
                            d = ((Scalar) o).getUnsignedValue();
                            ++sc;
                        } else if (o instanceof Register)
                            ++regs;
                    }
                    if (d != null && sc == 1 && regs == 1 && disps.contains(d)) {
                        Register target = (Register) dst[0];
                        Instruction n = ins.getNext();
                        for (int k = 0; k < 24 && n != null; ++k, n = n.getNext()) {
                            if (n.getMnemonicString().equalsIgnoreCase("CALL")) {
                                Object[] co = n.getOpObjects(0);
                                if (co.length == 1 && co[0] instanceof Register && co[0].equals(target)) {
                                    Function f = getFunctionContaining(n.getAddress());
                                    String fn = f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint();
                                    hits.computeIfAbsent(d, kk -> new ArrayList<>())
                                        .add(n.getAddress() + "  " + ins + " ; " + n + "  in " + fn);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            if (!ins.getMnemonicString().equalsIgnoreCase("CALL"))
                continue;
            ++calls;
            Object[] objs = ins.getOpObjects(0);
            Register reg = null;
            Long disp = null;
            int scalars = 0;
            for (Object o : objs) {
                if (o instanceof Register)
                    reg = (Register) o;
                else if (o instanceof Scalar) {
                    disp = ((Scalar) o).getUnsignedValue();
                    ++scalars;
                }
            }
            if (reg == null || disp == null || scalars != 1 || !disps.contains(disp))
                continue;
            String rep = ins.toString();
            if (!rep.contains("["))
                continue;
            Function f = getFunctionContaining(ins.getAddress());
            String fn = f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint();
            hits.computeIfAbsent(disp, k -> new ArrayList<>()).add(ins.getAddress() + "  " + rep + "  in " + fn);
        }
        pw.println("CALL instructions: " + calls);
        for (Map.Entry<Long, List<String>> e : hits.entrySet()) {
            pw.println("=== +0x" + Long.toHexString(e.getKey()) + ": " + e.getValue().size() + " calls");
            for (String s : e.getValue())
                pw.println("  " + s);
        }
        pw.close();
        println("Wrote " + out);
    }
}
