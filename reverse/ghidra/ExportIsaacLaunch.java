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

public class ExportIsaacLaunch extends GhidraScript {
    @Override public void run() throws Exception {
        if (!"3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b".equalsIgnoreCase(currentProgram.getExecutableSHA256()))
            throw new IllegalArgumentException("Expected original J460");
        Path out = Paths.get(getScriptArgs()[0]); Files.createDirectories(out);
        Map<Address, Function> selected = new LinkedHashMap<>();
        List<String> index = new ArrayList<>(); index.add("string\treference\tfunction");
        for (String needle : new String[]{"GetUserProfileDirectoryA", "USERPROFILE", "savedatapath.txt",
                "--localhost_match", "--networktest", "--load-room=", "PauseOnFocusLost"}) {
            Address found = currentProgram.getMemory().findBytes(currentProgram.getMinAddress(),
                needle.getBytes(StandardCharsets.US_ASCII), null, true, monitor);
            if (found == null) continue;
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(found);
            while (refs.hasNext()) {
                Address ref = refs.next().getFromAddress(); Function f = getFunctionContaining(ref);
                index.add(needle + "\t" + ref + "\t" + (f == null ? "" : f.getEntryPoint()));
                if (f != null) selected.put(f.getEntryPoint(), f);
            }
        }
        DecompInterface decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("decompiler failed");
            for (Function f : selected.values()) {
                DecompileResults result = decompiler.decompileFunction(f, 30, monitor);
                boolean ok = result.decompileCompleted() && result.getDecompiledFunction() != null;
                Files.write(out.resolve(f.getEntryPoint() + ".c"),
                    (ok ? result.getDecompiledFunction().getC() : "// " + result.getErrorMessage()).getBytes(StandardCharsets.UTF_8));
            }
        } finally { decompiler.dispose(); }
        Files.write(out.resolve("anchors.tsv"), index, StandardCharsets.UTF_8);
        println("Launch candidates exported: " + selected.size());
    }
}
