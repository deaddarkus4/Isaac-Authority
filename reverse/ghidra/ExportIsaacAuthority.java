// J460-only static candidates for authoritative-state adapter research.
// @category Isaac
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.ReferenceIterator;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class ExportIsaacAuthority extends GhidraScript {
    @Override public void run() throws Exception {
        if (!"3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b".equalsIgnoreCase(currentProgram.getExecutableSHA256()))
            throw new IllegalArgumentException("Expected original J460");
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Expected output directory");
        Path out = Paths.get(args[0]); Files.createDirectories(out);
        Map<Address, Function> selected = new LinkedHashMap<>();
        List<String> index = new ArrayList<>();
        index.add("evidence\treference\tfunction\tdecompiled");
        // Includes leading spaces where they belong to the actual C string.
        for (String needle : new String[] {"%04d: %d.%d.%d pre(", " PostVelocity: (",
                "[Frame: %d] Initialized player with Variant"}) {
            Address found = currentProgram.getMemory().findBytes(currentProgram.getMinAddress(),
                needle.getBytes(StandardCharsets.US_ASCII), null, true, monitor);
            if (found == null) continue;
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(found);
            while (refs.hasNext()) {
                Address ref = refs.next().getFromAddress(); Function f = getFunctionContaining(ref);
                index.add(needle + "\t" + ref + "\t" + (f == null ? "" : f.getEntryPoint()) + "\t");
                if (f != null) selected.put(f.getEntryPoint(), f);
            }
        }
        // Input record lookup and insertion, statically called by J460 broadcast/receive.
        // Additional entries checked against x86 prologues/signatures in the original image:
        // Game::GetPlayer/GetNumPlayers; framestate formatter; entity initialization.
        for (long rva : new long[] {0x501190L, 0x501070L, 0x17870L, 0x178d0L,
                0x4fe3a0L, 0x4ffc70L, 0x2a93a0L, 0x382af0L}) {
            Address entry = currentProgram.getImageBase().add(rva);
            Function f = getFunctionAt(entry);
            if (f == null && getFunctionContaining(entry) == null) {
                disassemble(entry);
                f = createFunction(entry, null);
            }
            if (f != null) selected.put(f.getEntryPoint(), f);
        }
        // One caller level provides actual state-collection paths; cap the export.
        for (Function f : new ArrayList<Function>(selected.values())) {
            for (Function caller : f.getCallingFunctions(monitor)) {
                if (selected.size() >= 16) break;
                selected.put(caller.getEntryPoint(), caller);
                index.add("caller_of_" + f.getEntryPoint() + "\t\t" + caller.getEntryPoint() + "\t");
            }
        }
        DecompInterface decompiler = new DecompInterface();
        int success = 0;
        try {
            if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("decompiler failed");
            for (Function f : selected.values()) {
                monitor.checkCancelled();
                DecompileResults result = decompiler.decompileFunction(f, 25, monitor);
                boolean ok = result.decompileCompleted() && result.getDecompiledFunction() != null;
                if (ok) ++success;
                Files.write(out.resolve(f.getEntryPoint() + ".c"),
                    (ok ? result.getDecompiledFunction().getC() : "// " + result.getErrorMessage()).getBytes(StandardCharsets.UTF_8));
                index.add("export\t\t" + f.getEntryPoint() + "\t" + ok);
            }
        } finally { decompiler.dispose(); }
        Files.write(out.resolve("anchors.tsv"), index, StandardCharsets.UTF_8);
        if (success == 0) throw new IllegalStateException("No functions exported");
        Files.write(out.resolve("export-complete.txt"),
            ("decompiled=" + success + "\nruntimeValidated=false\n").getBytes(StandardCharsets.UTF_8));
        println("Authority candidates exported: " + success);
    }
}
