// Gives every listed function the parameters it really takes: the argument registers it reads before writing them
// (tools/decomp/livein.py), as custom storage, so Ghidra's C passes and names exactly those. Its own guesses drop
// arguments callers set up and miss ones functions read (in_r5 and friends). The return is left as it was.
//
// Arguments: a file of lines "ADDRESS r3 r4 f1 ..." (livein.py --ghidra). Writes to the project.
//@category FM2

import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DoubleDataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.SourceType;

public class ApplyParamsFm2 extends GhidraScript {
    @Override
    protected void run() throws Exception {
        List<String> lines = Files.readAllLines(new File(getScriptArgs()[0]).toPath());
        int done = 0, failed = 0, n = 0;
        final String convention = currentProgram.getCompilerSpec().getDefaultCallingConvention().getName();
        println("FM2: calling convention " + convention);
        int tx = currentProgram.startTransaction("FM2 parameters");
        try {
            for (String line : lines) {
                String[] f = line.trim().split("\\s+");
                if (f.length == 0 || f[0].isEmpty()) continue;
                Function fn = getFunctionAt(toAddr(Long.parseLong(f[0], 16)));
                if (fn == null) continue;
                List<Parameter> params = new ArrayList<>();
                try {
                    for (int i = 1; i < f.length; i++) {
                        boolean flt = f[i].startsWith("f");
                        Register reg = currentProgram.getRegister(f[i]);
                        // an integer in the low 4 bytes of its 8-byte register (big-endian: the last four). Where
                        // the code uses all 64 bits Ghidra has to guess the top half (it cast one to longlong, and
                        // 8237E0D8 went wrong): lift.py leaves those out. Whole registers read better as Ghidra C
                        // full of x._4_4_.
                        VariableStorage at = flt ? new VariableStorage(currentProgram, reg)
                            : new VariableStorage(currentProgram, new Varnode(reg.getAddress().add(4), 4));
                        DataType t = flt ? DoubleDataType.dataType : Undefined4DataType.dataType;
                        params.add(new ParameterImpl("param_" + f[i], t, at, currentProgram, SourceType.USER_DEFINED));
                    }
                    fn.replaceParameters(params, FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
                    // the compiler spec's convention, so Ghidra still knows what a call destroys (r3-r12 ...): left
                    // unknown, it carried registers across calls (in_r6 for garbage passed on)
                    fn.setCallingConvention(convention);
                    done++;
                } catch (Exception e) {
                    if (failed++ < 10) println("FM2: " + f[0] + ": " + e.getMessage());
                }
                if (++n % 5000 == 0) {
                    currentProgram.endTransaction(tx, true);
                    tx = currentProgram.startTransaction("FM2 parameters");
                    println("FM2: " + n + " functions");
                }
            }
        } finally {
            currentProgram.endTransaction(tx, true);
        }
        println("FM2: parameters set on " + done + " functions, " + failed + " failed");
    }
}
