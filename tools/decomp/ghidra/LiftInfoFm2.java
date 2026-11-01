// Decompiles the listed functions with what tools/decomp/lift.py needs to turn their C into code that builds:
// each parameter's register, the return's, and the type and address of every global the C names.
//
// Arguments: a file of addresses (one hex address a line), the output (JSON lines, one function a line).
//@category FM2

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;

public class LiftInfoFm2 extends GhidraScript {
    static String q(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c == '\n') b.append("\\n");
            else if (c == '\r') continue;
            else if (c == '\t') b.append("\\t");
            else if (c < 32) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }

    static String sym(HighSymbol s) {
        DataType t = s.getDataType();
        String storage;
        try {
            storage = s.getStorage().toString();
        } catch (Exception e) {
            storage = "?";
        }
        return "{\"name\":" + q(s.getName()) + ",\"type\":" + q(t == null ? "?" : t.getDisplayName()) + ",\"size\":"
            + (t == null ? 0 : t.getLength()) + ",\"storage\":" + q(storage) + "}";
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        List<Function> funcs = new ArrayList<>();
        for (String line : Files.readAllLines(new File(args[0]).toPath())) {
            line = line.trim();
            if (line.isEmpty()) continue;
            Function f = getFunctionAt(toAddr(Long.parseLong(line, 16)));
            if (f != null) funcs.add(f);
        }
        println("FM2: lift info for " + funcs.size() + " functions; pointer size "
            + currentProgram.getDataTypeManager().getDataOrganization().getPointerSize());

        DecompileConfigurer config = d -> {
            DecompileOptions o = new DecompileOptions();
            o.grabFromProgram(currentProgram);
            d.setOptions(o);
            d.toggleCCode(true);
            d.toggleSyntaxTree(true);
            d.setSimplificationStyle("decompile");
        };
        DecompilerCallback<String> cb = new DecompilerCallback<>(currentProgram, config) {
            @Override
            public String process(DecompileResults r, ghidra.util.task.TaskMonitor m) {
                Function f = r.getFunction();
                String addr = f.getEntryPoint().toString().toUpperCase();
                HighFunction hf = r.getHighFunction();
                if (!r.decompileCompleted() || hf == null || r.getDecompiledFunction() == null)
                    return "{\"addr\":" + q(addr) + ",\"error\":" + q(String.valueOf(r.getErrorMessage())) + "}";
                StringBuilder b = new StringBuilder();
                b.append("{\"addr\":").append(q(addr)).append(",\"c\":").append(q(r.getDecompiledFunction().getC()));
                var proto = hf.getFunctionPrototype();
                DataType rt = proto.getReturnType();
                String rs;
                try {
                    rs = proto.getReturnStorage().toString();
                } catch (Exception e) {
                    rs = "?";
                }
                b.append(",\"ret\":{\"type\":").append(q(rt == null ? "?" : rt.getDisplayName())).append(",\"size\":")
                    .append(rt == null ? 0 : rt.getLength()).append(",\"storage\":").append(q(rs)).append('}');
                var lsm = hf.getLocalSymbolMap();
                b.append(",\"params\":[");
                for (int i = 0; i < lsm.getNumParams(); i++)
                    b.append(i > 0 ? "," : "").append(sym(lsm.getParamSymbol(i)));
                b.append("],\"locals\":[");
                boolean first = true;
                for (Iterator<HighSymbol> it = lsm.getSymbols(); it.hasNext();) {
                    HighSymbol s = it.next();
                    if (s.isParameter()) continue;
                    b.append(first ? "" : ",").append(sym(s));
                    first = false;
                }
                b.append("],\"globals\":[");
                first = true;
                for (Iterator<HighSymbol> it = hf.getGlobalSymbolMap().getSymbols(); it.hasNext();) {
                    b.append(first ? "" : ",").append(sym(it.next()));
                    first = false;
                }
                return b.append("]}").toString();
            }
        };
        int n = 0;
        try (PrintWriter out = new PrintWriter(new File(args[1]), "UTF-8")) {
            for (String line : ParallelDecompiler.decompileFunctions(cb, funcs, monitor))
                if (line != null) {
                    out.println(line);
                    n++;
                }
        } finally {
            cb.dispose();
        }
        println("FM2: wrote " + n + " functions to " + args[1]);
    }
}
