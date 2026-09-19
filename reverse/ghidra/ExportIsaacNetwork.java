// Export functions referenced by J460 networking and item-event strings.
// @category Isaac

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class ExportIsaacNetwork extends GhidraScript {
    private static final String[] NEEDLES = {
        "NetInputDevice::broadcast_frame_input",
        "NetInputDevice::ProcessInputMessage",
        "NetDesyncHandler::process_desync_confirm_message",
        "NetDesyncHandler::process_desync_recovery_start_message",
        "resolutionPlan = SYNC_PLAN_DROP",
        "Adding trinket",
        "item queue flush",
        "OnlineInputDelay=%d"
    };

    @Override public void run() throws Exception {
        if (!"3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b".equalsIgnoreCase(currentProgram.getExecutableSHA256()))
            throw new IllegalArgumentException("Expected the verified original J460 program");
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Expected one output-directory argument");
        Path output = Paths.get(args[0]).toAbsolutePath();
        Files.createDirectories(output);
        DecompInterface decompiler = new DecompInterface();
        if (!decompiler.openProgram(currentProgram)) throw new IOException("Could not initialize decompiler");
        Set<Address> exported = new HashSet<>();
        Map<Address, Boolean> decompiled = new HashMap<>();
        try (BufferedWriter index = Files.newBufferedWriter(output.resolve("anchors.tsv"), StandardCharsets.UTF_8)) {
            index.write("needle\tstring_address\treference_address\tfunction_entry\tfunction_name\tdecompiled\n");
            for (String needle : NEEDLES) {
                byte[] bytes = needle.getBytes(StandardCharsets.US_ASCII);
                Address next = currentProgram.getMinAddress();
                while (!monitor.isCancelled()) {
                    Address found = currentProgram.getMemory().findBytes(next, bytes, null, true, monitor);
                    if (found == null) break;
                    // Log strings may contain a prefix before the searched fragment.
                    Address start = found;
                    for (int i = 0; i < 160 && start.compareTo(currentProgram.getMinAddress()) > 0; i++) {
                        Address prev = start.subtract(1);
                        if (!currentProgram.getMemory().contains(prev)) break;
                        int ch = currentProgram.getMemory().getByte(prev) & 0xff;
                        if (ch < 32 || ch > 126) break;
                        start = prev;
                    }
                    ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(start);
                    boolean any = false;
                    while (refs.hasNext()) {
                        any = true;
                        Reference ref = refs.next();
                        Function fn = getFunctionContaining(ref.getFromAddress());
                        boolean success = false;
                        if (fn != null && exported.add(fn.getEntryPoint())) {
                            DecompileResults result = decompiler.decompileFunction(fn, 90, monitor);
                            success = result.decompileCompleted() && result.getDecompiledFunction() != null;
                            decompiled.put(fn.getEntryPoint(), success);
                            String text = success ? result.getDecompiledFunction().getC() : "// Decompilation failed: " + result.getErrorMessage();
                            String filename = fn.getEntryPoint().toString() + ".c";
                            Files.write(output.resolve(filename), text.getBytes(StandardCharsets.UTF_8));
                        }
                        if (fn != null) success = decompiled.getOrDefault(fn.getEntryPoint(), false);
                        index.write(needle + "\t" + start + "\t" + ref.getFromAddress() + "\t" +
                            (fn == null ? "" : fn.getEntryPoint()) + "\t" +
                            (fn == null ? "" : fn.getName()) + "\t" + success + "\n");
                        index.flush();
                    }
                    if (!any) index.write(needle + "\t" + start + "\t\t\t\tfalse\n");
                    next = found.add(bytes.length);
                    if (next.compareTo(currentProgram.getMaxAddress()) > 0) break;
                }
            }
            // Follow-up candidates called from the trinket/queue path. These are
            // exported for analysis, not treated as verified hook signatures.
            for (long rva : new long[] { 0x6112c0L, 0x3ad860L, 0x3584b0L, 0x3afc20L }) {
                Function fn = getFunctionAt(currentProgram.getImageBase().add(rva));
                if (fn == null || !exported.add(fn.getEntryPoint())) continue;
                DecompileResults result = decompiler.decompileFunction(fn, 90, monitor);
                boolean success = result.decompileCompleted() && result.getDecompiledFunction() != null;
                decompiled.put(fn.getEntryPoint(), success);
                String text = success ? result.getDecompiledFunction().getC() : "// Decompilation failed: " + result.getErrorMessage();
                Files.write(output.resolve(fn.getEntryPoint().toString() + ".c"), text.getBytes(StandardCharsets.UTF_8));
                index.write("trinket_path_followup\t\t\t" + fn.getEntryPoint() + "\t" + fn.getName() + "\t" + success + "\n");
            }
        } finally {
            decompiler.dispose();
        }
        if (monitor.isCancelled()) throw new IOException("Export was cancelled");
        long successful = decompiled.values().stream().filter(Boolean::booleanValue).count();
        if (successful == 0) throw new IOException("No candidate functions were decompiled; inspect the analysis log");
        Files.write(output.resolve("export-complete.txt"),
            ("candidateFunctions=" + exported.size() + "\ndecompiledFunctions=" + successful + "\n").getBytes(StandardCharsets.UTF_8));
        println("Exported " + exported.size() + " candidate functions to " + output);
    }
}
