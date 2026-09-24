// Find MSVC's stack probe (__alloca_probe / _chkstk) by its code and give it the "alloca_probe" call fixup,
// so functions with big frames decompile with their real arguments instead of in_stack_XXXX.
// Pattern: push ecx; cmp eax, 0x1000; lea ecx, [esp+8]   (51 3D 00 10 00 00 8D 4C 24 08)
// Run it as the first -postScript, before any export.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class FindProbe extends GhidraScript {
    @Override
    public void run() throws Exception {
        byte[] pat = { 0x51, 0x3d, 0x00, 0x10, 0x00, 0x00, (byte) 0x8d, 0x4c, 0x24, 0x08 };
        Memory mem = currentProgram.getMemory();
        int found = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            byte[] b = new byte[pat.length];
            try {
                mem.getBytes(f.getEntryPoint(), b);
            } catch (MemoryAccessException e) {
                continue;
            }
            boolean ok = true;
            for (int i = 0; i < pat.length; i++) ok &= b[i] == pat[i];
            if (!ok) continue;
            f.setName(found == 0 ? "__alloca_probe" : "__alloca_probe_" + found, SourceType.USER_DEFINED);
            f.setCallFixup("alloca_probe");
            println("stack probe at " + f.getEntryPoint() + ", call fixup " + f.getCallFixup());
            found++;
        }
        if (found == 0) println("no stack probe found");
    }
}
