// Lists every instruction with primary opcode 4, 5 or 6 (AltiVec and VMX128) as Ghidra decodes it, to check the
// Xenon language against XenonRecomp's decoding (vmx128.py check).
//
// Argument: output file. Lines: address, instruction word, Ghidra's text (or "?" where it doesn't decode).
//@category FM2

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryAccessException;

public class DumpVectorFm2 extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String out = getScriptArgs()[0];
        int n = 0, bad = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                for (Address a : f.getBody().getAddresses(true)) {
                    if (a.getOffset() % 4 != 0) continue;
                    int word;
                    try { word = getInt(a); } catch (MemoryAccessException e) { continue; }
                    int op = (word >>> 26);
                    if (op != 4 && op != 5 && op != 6) continue;
                    Instruction i = getInstructionAt(a);
                    String text = i == null ? "?" : i.toString();
                    if (i == null) bad++;
                    w.printf("%08X\t%08X\t%s%n", a.getOffset(), word, text);
                    n++;
                }
            }
        }
        println("FM2: " + n + " vector instructions listed, " + bad + " not decoded");
    }
}
