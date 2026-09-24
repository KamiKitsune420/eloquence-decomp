// Fix types Ghidra guessed wrong, then decompile the changed functions and their callers into one file.
//
//   args: <outFile> <fix> ... [-- <addr> ...]      (@file: read these from a file, one per line)
//     <addr>:<param index>:<type>     one parameter; type is "char*" or "int"
//     <addr>:sig:<C prototype>        the whole signature, e.g.
//                                     "0x1002b690:sig:void __thiscall f(char *a, char *b)"
//   The addresses after "--" are decompiled too (without changes).
//   Examples: FUN_10076c30's text argument came out as IMalloc* (it is later handed to IMalloc::Free),
//   which turns every character read into "->lpVtbl" member accesses; FUN_1002b690 (a thiscall with
//   four string arguments) came out as taking one char.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.util.parser.FunctionSignatureParser;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Retype extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<String> al = new ArrayList<>();
        for (String s : getScriptArgs()) {
            if (s.startsWith("@")) {                  // fixes from a file, one per line (cmd.exe mangles prototypes)
                for (String l : java.nio.file.Files.readAllLines(new File(s.substring(1)).toPath()))
                    if (!l.trim().isEmpty()) al.add(l.trim());
            } else al.add(s);
        }
        String[] a = al.toArray(new String[0]);
        List<Function> out = new ArrayList<>();
        boolean plain = false;
        for (int i = 1; i < a.length; i++) {
            if (a[i].equals("--")) { plain = true; continue; }
            String[] p = a[i].split(":", 3);
            Function f = getFunctionAt(toAddr(Long.decode(p[0])));
            if (f == null) { println("no function at " + p[0]); continue; }
            if (!out.contains(f)) out.add(f);
            if (plain) continue;
            if (p[1].equals("sig")) {
                // the parser takes no calling convention: strip it and set it afterwards (a __thiscall
                // prototype lists the parameters after `this`)
                String proto = p[2], conv = null;
                for (String c : new String[] { "__thiscall", "__stdcall", "__cdecl", "__fastcall" })
                    if (proto.contains(c)) { conv = c; proto = proto.replace(c, " "); }
                FunctionSignatureParser parser = new FunctionSignatureParser(currentProgram.getDataTypeManager(), null);
                FunctionDefinitionDataType sig = parser.parse(f.getSignature(), proto);
                ApplyFunctionSignatureCmd cmd = new ApplyFunctionSignatureCmd(f.getEntryPoint(), sig, SourceType.USER_DEFINED);
                if (!cmd.applyTo(currentProgram)) println("signature not applied: " + cmd.getStatusMsg());
                if (conv != null) f.setCallingConvention(conv);
                println(f.getName() + " -> " + f.getSignature().getPrototypeString());
                for (Function c : f.getCallingFunctions(monitor))
                    if (!out.contains(c)) out.add(c);
                continue;
            }
            int idx = Integer.parseInt(p[1]);
            DataType t = p[2].equals("char*") ? new PointerDataType(CharDataType.dataType) : IntegerDataType.dataType;
            Parameter prm = f.getParameter(idx);
            if (prm == null) { println("no parameter " + idx + " in " + f.getName()); continue; }
            prm.setDataType(t, SourceType.USER_DEFINED);
            println(f.getName() + " param " + idx + " -> " + prm.getDataType().getName());
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (Function f : out) {
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                w.println("// " + f.getName() + " @ " + f.getEntryPoint());
                if (r != null && r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
                else w.println("// decompile failed\n");
            }
        }
        println("decompiled " + out.size() + " functions");
    }
}
