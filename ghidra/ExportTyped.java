// Like CallTargets.java (recover the functions auto-analysis missed, then decompile everything), and also
// record the type the decompiler gave every global it printed. Ghidra prints `&DAT_x + n` and
// `(&DAT_x)[n]` scaled by that type, which the C text alone does not show, so a translator needs it.
//
// Outputs in <outDir>/<program>/:
//   _all.c        every function, decompiled
//   globals.tsv   name <tab> address <tab> size in bytes <tab> type name, for every global referenced
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.pcode.*;
import java.io.*;
import java.util.*;

public class ExportTyped extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = null;
        for (MemoryBlock b : mem.getBlocks())
            if (b.getName().equals(".text")) text = b;
        long lo = text.getStart().getOffset(), hi = text.getEnd().getOffset();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();

        Set<Long> targets = new TreeSet<>();
        for (MemoryBlock b : mem.getBlocks()) {
            String n = b.getName();
            if (!n.equals(".rdata") && !n.equals(".data")) continue;
            for (Address a = b.getStart(); a.compareTo(b.getEnd().subtract(4)) < 0; a = a.add(4)) {
                long v;
                try { v = mem.getInt(a) & 0xFFFFFFFFL; } catch (MemoryAccessException e) { continue; }
                if (v >= lo && v <= hi) targets.add(v);
            }
        }
        byte[] code = new byte[(int) (hi - lo + 1)];
        mem.getBytes(text.getStart(), code);
        for (int i = 0; i + 5 < code.length; i++) {
            if ((code[i] & 0xFF) != 0xE8) continue;
            long rel = (code[i + 1] & 0xFFL) | ((code[i + 2] & 0xFFL) << 8)
                     | ((code[i + 3] & 0xFFL) << 16) | ((long) code[i + 4] << 24);
            long t = lo + i + 5 + rel;
            if (t >= lo && t <= hi) targets.add(t);
        }
        int made = 0;
        for (long t : targets) {
            if (monitor.isCancelled()) break;
            Address a = sp.getAddress(t);
            if (getFunctionAt(a) != null) continue;
            if (getInstructionAt(a) == null) disassemble(a);
            if (getInstructionAt(a) == null) continue;
            if (createFunction(a, null) != null) made++;
        }
        println("created " + made + " new functions");

        String name = currentProgram.getName().replace(".dll", "").replace(".DLL", "");
        File dir = new File(outDir, name);
        dir.mkdirs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        Map<String, String> globals = new TreeMap<>();
        int n = 0;
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(new File(dir, "_all.c")), 1 << 20))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                w.println("// " + f.getName() + " @ " + f.getEntryPoint());
                if (r != null && r.decompileCompleted()) {
                    w.println(r.getDecompiledFunction().getC());
                    collect(r.getCCodeMarkup(), globals);
                } else w.println("// decompile failed\n");
                n++;
            }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(dir, "globals.tsv")))) {
            for (Map.Entry<String, String> e : globals.entrySet()) w.println(e.getKey() + "\t" + e.getValue());
        }
        println("exported " + n + " functions, " + globals.size() + " globals, from " + name);
    }

    private void collect(ClangNode node, Map<String, String> out) {
        if (node == null) return;
        if (node instanceof ClangVariableToken) {
            ClangVariableToken t = (ClangVariableToken) node;
            HighVariable hv = t.getHighVariable();
            if (hv instanceof HighGlobal) {
                HighSymbol s = hv.getSymbol();
                DataType dt = hv.getDataType();
                String nm = s != null ? s.getName() : t.getText();
                Address a = s != null && s.getStorage() != null && s.getStorage().getMinAddress() != null
                          ? s.getStorage().getMinAddress() : null;
                String v = (a != null ? a.toString() : "?") + "\t" + (dt != null ? dt.getLength() : -1)
                         + "\t" + (dt != null ? dt.getDisplayName() : "?");
                out.putIfAbsent(nm, v);
            }
        }
        if (node instanceof ClangTokenGroup) {
            ClangTokenGroup g = (ClangTokenGroup) node;
            for (int i = 0; i < g.numChildren(); i++) collect(g.Child(i), out);
        }
    }
}
