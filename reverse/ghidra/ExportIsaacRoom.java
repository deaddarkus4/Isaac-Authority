// @category Isaac
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.ReferenceIterator;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

// Arguments: OUTPUT_DIRECTORY, then "name:LuaMethod" (resolved through its binding registration) or a hex RVA.
public class ExportIsaacRoom extends GhidraScript {
    @Override public void run() throws Exception {
        if (!"3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b".equalsIgnoreCase(currentProgram.getExecutableSHA256()))
            throw new IllegalArgumentException("Expected original J460");
        String[] args = getScriptArgs(); Path out = Paths.get(args[0]); Files.createDirectories(out);
        long imageBase = currentProgram.getImageBase().getOffset();
        long imageEnd = currentProgram.getMaxAddress().getOffset();
        List<String> index = new ArrayList<>(); index.add("name\tstring\treference\tfunctionRva\tcontainingFunction");
        Map<Long, String> targets = new LinkedHashMap<>();
        for (int i = 1; i < args.length; ++i) {
            if (!args[i].startsWith("name:")) { targets.putIfAbsent(Long.decode(args[i]), "rva"); continue; }
            String name = args[i].substring(5);
            byte[] needle = ("\0" + name + "\0").getBytes(StandardCharsets.US_ASCII);
            Address from = currentProgram.getMinAddress();
            while (from != null) {
                Address hit = currentProgram.getMemory().findBytes(from, needle, null, true, monitor);
                if (hit == null) break;
                Address text = hit.add(1);
                ReferenceIterator iterator = currentProgram.getReferenceManager().getReferencesTo(text);
                while (iterator.hasNext()) {
                    Address reference = iterator.next().getFromAddress();
                    Instruction push = getInstructionAt(reference);
                    Instruction previous = push == null ? null : push.getPrevious();
                    String resolved = "";
                    // LuaBridge registration: PUSH member-function, PUSH name, MOV ECX, CALL add-function.
                    if (previous != null && "PUSH".equals(previous.getMnemonicString()) && previous.getNumOperands() == 1) {
                        Object[] operand = previous.getOpObjects(0);
                        if (operand.length == 1 && operand[0] instanceof Scalar) {
                            long value = ((Scalar) operand[0]).getUnsignedValue();
                            if (value > imageBase && value < imageEnd) {
                                resolved = "0x" + Long.toHexString(value - imageBase);
                                targets.putIfAbsent(value - imageBase, name);
                            }
                        }
                    }
                    Function container = getFunctionContaining(reference);
                    index.add(name + "\t" + text + "\t" + reference + "\t" + resolved + "\t" +
                        (container == null ? "" : container.getEntryPoint().toString()));
                }
                from = hit.add(needle.length);
            }
        }
        Files.write(out.resolve("anchors.tsv"), index, StandardCharsets.UTF_8);
        DecompInterface decompiler = new DecompInterface();
        try {
            decompiler.openProgram(currentProgram);
            for (Map.Entry<Long, String> target : targets.entrySet()) {
                Address at = currentProgram.getImageBase().add(target.getKey());
                Function function = getFunctionAt(at);
                if (function == null) { disassemble(at); function = createFunction(at, null); }
                if (function == null) continue;
                List<String> listing = new ArrayList<>();
                Instruction instruction = getInstructionAt(at);
                for (int step = 0; step < 120 && instruction != null && function.getBody().contains(instruction.getAddress());
                     ++step, instruction = instruction.getNext())
                    listing.add(instruction.getAddress() + " " + instruction.toString());
                Files.write(out.resolve(at + ".asm"), listing, StandardCharsets.UTF_8);
                DecompileResults result = decompiler.decompileFunction(function, 60, monitor);
                String header = "// " + target.getValue() + " RVA 0x" + Long.toHexString(target.getKey()) + "\n";
                Files.write(out.resolve(at + ".c"), (header + (result.decompileCompleted() && result.getDecompiledFunction() != null ?
                    result.getDecompiledFunction().getC() : "// " + result.getErrorMessage())).getBytes(StandardCharsets.UTF_8));
                println("Room export " + at + " " + target.getValue() + " " + result.decompileCompleted());
            }
        } finally { decompiler.dispose(); }
        Files.write(out.resolve("export-complete.txt"), Collections.singletonList("complete"), StandardCharsets.UTF_8);
    }
}
