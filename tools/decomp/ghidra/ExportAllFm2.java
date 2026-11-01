// Decompiles every function in the FM2 program into C files, in parallel, with a quality report.
//
// Arguments: output folder (default D:/ghidra/export). Writes <folder>/c/<kind>/<8 hex>.c per function, grouped
// into one file per 64 KB of address space (<kind>/<8 hex of the block>.c) to keep the file count sane, and
// <folder>/report.tsv: address, name, kind, status (ok / warnings / failed), C lines, and the warnings.
//@category FM2

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class ExportAllFm2 extends GhidraScript {
    record Out(String addr, String name, String kind, String status, int lines, String warnings, String c) {}

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0] : "D:/ghidra/export");
        new File(dir, "c").mkdirs();
        List<Function> funcs = new ArrayList<>();
        currentProgram.getFunctionManager().getFunctions(true).forEach(funcs::add);
        println("FM2: decompiling " + funcs.size() + " functions");

        Map<String, Out> results = new ConcurrentHashMap<>();
        DecompileConfigurer config = d -> {
            DecompileOptions o = new DecompileOptions();
            o.grabFromProgram(currentProgram);
            d.setOptions(o);
            d.toggleCCode(true);
            d.toggleSyntaxTree(false);
            d.setSimplificationStyle("decompile");
        };
        DecompilerCallback<Out> cb = new DecompilerCallback<>(currentProgram, config) {
            @Override
            public Out process(DecompileResults r, ghidra.util.task.TaskMonitor m) {
                Function f = r.getFunction();
                String addr = f.getEntryPoint().toString().toUpperCase();
                String plate = currentProgram.getListing().getComment(ghidra.program.model.listing.CommentType.PLATE, f.getEntryPoint());
                String kind = plate != null ? plate.split(" ")[0] : "unknown";
                if (!r.decompileCompleted() || r.getDecompiledFunction() == null)
                    return new Out(addr, f.getName(true), kind, "failed", 0, r.getErrorMessage().replace('\n', ' '), "");
                String c = r.getDecompiledFunction().getC();
                List<String> warn = new ArrayList<>();
                for (String line : c.split("\n")) {
                    int i = line.indexOf("WARNING: ");
                    if (i >= 0 && !line.contains("replaced with injection")) warn.add(line.substring(i + 9).replace("*/", "").trim());
                }
                String status = c.contains("halt_baddata") || c.contains("Bad instruction") ? "baddata"
                    : warn.isEmpty() ? "ok" : "warnings";
                return new Out(addr, f.getName(true), kind, status, c.split("\n").length, String.join(" | ", warn), c);
            }
        };
        try {
            for (Out o : ParallelDecompiler.decompileFunctions(cb, funcs, monitor))
                if (o != null) results.put(o.addr(), o);
        } finally {
            cb.dispose();
        }

        // group into one file per kind and 64 KB block
        Map<String, StringBuilder> files = new TreeMap<>();
        try (PrintWriter rep = new PrintWriter(new File(dir, "report.tsv"))) {
            rep.println("address\tname\tkind\tstatus\tlines\twarnings");
            for (Out o : new TreeMap<>(results).values()) {
                rep.println(String.join("\t", o.addr(), o.name(), o.kind(), o.status(), "" + o.lines(), o.warnings()));
                String key = o.kind() + "/" + o.addr().substring(0, 4) + "0000.c";
                files.computeIfAbsent(key, k -> new StringBuilder()).append("// ---- ").append(o.addr()).append(' ')
                    .append(o.name()).append(" [").append(o.status()).append("]\n").append(o.c()).append("\n\n");
            }
        }
        for (var e : files.entrySet()) {
            File f = new File(new File(dir, "c"), e.getKey());
            f.getParentFile().mkdirs();
            try (PrintWriter w = new PrintWriter(f)) { w.print(e.getValue()); }
        }
        Map<String, Integer> by = new TreeMap<>();
        for (Out o : results.values()) by.merge(o.status(), 1, Integer::sum);
        println("FM2: exported " + results.size() + " functions into " + files.size() + " files: " + by);
    }
}
