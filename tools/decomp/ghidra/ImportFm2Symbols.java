// Creates FM2's functions from out/decomp/symbols.txt (symbols.py) and names them: one function per
// XenonRecomp function start, Class::vfN names in their class's namespace, and a plate comment with the kind
// (init, virtual, library, lua) and library. Run before analysis (-preScript) so analysis builds on them.
//
// Argument: the path to symbols.txt (default D:/fm2-port/out/decomp/symbols.txt).
//@category FM2

import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class ImportFm2Symbols extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        String path = args.length > 0 ? args[0] : "D:/fm2-port/out/decomp/symbols.txt";
        int made = 0, named = 0, failed = 0, lines = 0;
        println("FM2: heap limit " + Runtime.getRuntime().maxMemory() / (1 << 20) + " MB");
        helpers(args.length > 1 ? args[1] : "D:/fm2-port/config/fm2_tu4.toml");
        // its guesses at functions that never return are wrong here (it cut callers short after ordinary calls)
        setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = in.readLine()) != null) {
                // "821E0000     96 virtual           Class::vf3"  (address, size, kind, [lib], name)
                String[] f = line.trim().split("\\s+");
                if (f.length < 4) continue;
                Address a = toAddr(Long.parseLong(f[0], 16));
                String kind = f[2];
                String lib = f.length >= 5 ? f[3] : "";
                String name = f[f.length - 1];
                new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                Function fn = getFunctionAt(a);
                if (fn == null) {
                    fn = createFunction(a, null);
                    if (fn == null) { failed++; continue; }
                    made++;
                }
                if (!name.startsWith("sub_")) {
                    Namespace ns = currentProgram.getGlobalNamespace();
                    String leaf = name;
                    int cut = name.lastIndexOf("::");
                    if (cut > 0) {
                        ns = NamespaceUtils.createNamespaceHierarchy(name.substring(0, cut), null, currentProgram, SourceType.USER_DEFINED);
                        leaf = name.substring(cut + 2);
                    }
                    try {
                        fn.setParentNamespace(ns);
                        fn.setName(leaf, SourceType.USER_DEFINED);
                        named++;
                    } catch (Exception e) {
                        println("can't name " + f[0] + " " + name + ": " + e.getMessage());
                    }
                }
                setPlateComment(a, kind + (lib.isEmpty() ? "" : " (" + lib + ")"));
                if (monitor.isCancelled()) break;
                // commit in batches: one transaction over 50,000 functions runs the heap out
                if (++lines % 2000 == 0) {
                    end(true);
                    start();
                }
            }
        }
        println("FM2: " + made + " functions made, " + named + " named, " + failed + " failed");
        switches(args.length > 2 ? args[2] : "D:/fm2-port/config/fm2_switch_tables_tu4.toml");
    }

    /** XenonAnalyse's switch tables (the recompiler's config): Ghidra can't follow most of these jump tables
     * itself ("Could not recover jumptable"), so each bctr gets its targets as references and the decompiler an
     * override listing them. */
    void switches(String toml) throws Exception {
        String text = new String(java.nio.file.Files.readAllBytes(java.nio.file.Path.of(toml)));
        int n = 0;
        for (String blk : text.split("\\[\\[switch\\]\\]")) {
            java.util.regex.Matcher b = java.util.regex.Pattern.compile("base = 0x([0-9A-Fa-f]+)").matcher(blk);
            if (!b.find()) continue;
            long base = Long.parseLong(b.group(1), 16);
            java.util.List<Address> labels = new java.util.ArrayList<>();
            String list = blk.substring(blk.indexOf("labels = ["));
            java.util.regex.Matcher l = java.util.regex.Pattern.compile("0x([0-9A-Fa-f]+)").matcher(list);
            while (l.find()) {
                Address t = toAddr(Long.parseLong(l.group(1), 16));
                if (!labels.contains(t)) labels.add(t);
            }
            Address bctr = null;
            for (long a = base; a < base + 64; a += 4)
                if (getInt(toAddr(a)) == 0x4E800420) { bctr = toAddr(a); break; }
            Function fn = bctr == null ? null : getFunctionContaining(toAddr(base));
            if (bctr == null || fn == null) continue;
            for (Address t : labels) {
                currentProgram.getReferenceManager().addMemoryReference(bctr, t,
                    ghidra.program.model.symbol.RefType.COMPUTED_JUMP, SourceType.USER_DEFINED, 0);
                new DisassembleCommand(t, null, true).applyTo(currentProgram, monitor);
            }
            ghidra.app.cmd.function.CreateFunctionCmd.fixupFunctionBody(currentProgram, fn, monitor);
            new ghidra.program.model.pcode.JumpTable(bctr, new java.util.ArrayList<>(labels), true, 0).writeOverride(fn);
            n++;
        }
        println("FM2: " + n + " switch tables from XenonRecomp's config");
    }

    /** The compiler's register save/restore helpers (savegprlr_14..31 and the rest, from XenonRecomp's config):
     * prologues and epilogues jump into the middle of them, one entry per register. Each entry gets a call fixup
     * that does nothing: the saves and restores are bookkeeping, and as plain functions (or inlined, as their blr
     * then reads as the caller returning) they cut the caller's decompiled body short. */
    void helpers(String toml) throws Exception {
        new ghidra.program.database.SpecExtension(currentProgram).addReplaceCompilerSpecExtension(
            "<callfixup name=\"fm2_saverest\"><pcode><body><![CDATA[ r0 = r0; ]]></body></pcode></callfixup>", monitor);
        java.util.Map<String, Long> at = new java.util.HashMap<>();
        for (String line : java.nio.file.Files.readAllLines(java.nio.file.Path.of(toml))) {
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("^(\\w+)_address\\s*=\\s*0x([0-9A-Fa-f]+)").matcher(line.trim());
            if (m.find()) at.put(m.group(1), Long.parseLong(m.group(2), 16));
        }
        int n = 0;
        for (String kind : new String[] { "savegprlr", "restgprlr", "savefpr", "restfpr" }) {
            Long base = at.get(kind + "_14");
            if (base == null) continue;
            for (int r = 14; r <= 31; r++) {
                Address a = toAddr(base + (r - 14) * 4L);
                new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                Function fn = getFunctionAt(a);
                if (fn == null) fn = createFunction(a, kind + "_" + r);
                if (fn == null) continue;
                fn.setName(kind + "_" + r, SourceType.USER_DEFINED);
                fn.setNoReturn(false);
                fn.setCallFixup("fm2_saverest");
                n++;
            }
        }
        println("FM2: " + n + " register save/restore helpers given a no-op call fixup");
    }
}
