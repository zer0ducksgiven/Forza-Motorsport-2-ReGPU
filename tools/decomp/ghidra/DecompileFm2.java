// Writes Ghidra's decompiled C for the given functions to a file: a quick look at what the decompiler
// makes of FM2 without opening the GUI.
//
// Arguments: output file, then function addresses (hex, TU4).
//@category FM2

import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileFm2 extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Function fn = getFunctionAt(toAddr(Long.parseLong(args[i], 16)));
                if (fn == null) { out.println("// no function at " + args[i]); continue; }
                DecompileResults r = d.decompileFunction(fn, 120, monitor);
                out.println("// ---- " + args[i] + " " + fn.getName(true));
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
