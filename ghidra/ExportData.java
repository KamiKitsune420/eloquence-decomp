// Dump every defined data item in the listing: address, length, type, and for arrays the element
// length. The decompiler scales `&DAT_x + n` by the type the listing has at DAT_x.
//   <outDir>/<program>/data.tsv   address <tab> length <tab> element length <tab> type name
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportData extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        String name = currentProgram.getName().replace(".dll", "").replace(".DLL", "");
        File dir = new File(outDir, name);
        dir.mkdirs();
        int n = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(dir, "data.tsv")))) {
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                DataType dt = d.getDataType();
                int elem = d.getLength();
                if (dt instanceof Array) elem = ((Array) dt).getElementLength();
                w.println(d.getAddress() + "\t" + d.getLength() + "\t" + elem + "\t" + dt.getDisplayName());
                n++;
            }
        }
        println("exported " + n + " data items");
    }
}
