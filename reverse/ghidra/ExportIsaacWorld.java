// @category Isaac
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.ReferenceIterator;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class ExportIsaacWorld extends GhidraScript {
    @Override public void run() throws Exception {
        if (!"3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b".equalsIgnoreCase(currentProgram.getExecutableSHA256()))
            throw new IllegalArgumentException("Expected original J460");
        String[] args = getScriptArgs(); Path out = Paths.get(args[0]); Files.createDirectories(out);
        List<String> refs = new ArrayList<>();
        for (String name : new String[]{"GetRoomEntities", "Spawn", "Remove", "GetEntities", "GetCurrentRoomDesc", "ChangeRoom", "StartRoomTransition"}) {
            Address address = currentProgram.getMemory().findBytes(currentProgram.getMinAddress(),
                (name + "\0").getBytes(StandardCharsets.US_ASCII), null, true, monitor);
            if (address == null) continue;
            ReferenceIterator iterator = currentProgram.getReferenceManager().getReferencesTo(address);
            while (iterator.hasNext()) {
                Address reference = iterator.next().getFromAddress();
                refs.add("\nSTRING " + name + " at " + address + " reference " + reference);
                Instruction center = getInstructionAt(reference), first = center;
                if (first == null) continue;
                for (int i = 0; i < 14 && first.getPrevious() != null; ++i) first = first.getPrevious();
                Instruction instruction = first;
                for (int i = 0; i < 29 && instruction != null; ++i, instruction = instruction.getNext())
                    refs.add(instruction.getAddress() + " " + instruction.toString());
            }
        }
        Files.write(out.resolve("lua-bindings.txt"), refs, StandardCharsets.UTF_8);
        DecompInterface decompiler = new DecompInterface();
        try {
            decompiler.openProgram(currentProgram);
            for (int i = 1; i < args.length; ++i) {
                Address at = currentProgram.getImageBase().add(Long.decode(args[i]));
                Function function = getFunctionAt(at);
                if (function == null) { disassemble(at); function = createFunction(at, null); }
                if (function == null) continue;
                List<String> listing = new ArrayList<>();
                Instruction instruction = getInstructionAt(at);
                for (int step = 0; step < 80 && instruction != null; ++step, instruction = instruction.getNext())
                    listing.add(instruction.getAddress() + " " + instruction.toString());
                listing.add("END OF FUNCTION");
                instruction = getInstructionContaining(function.getBody().getMaxAddress());
                for (int step = 0; step < 25 && instruction != null; ++step, instruction = instruction.getPrevious())
                    listing.add(instruction.getAddress() + " " + instruction.toString());
                Files.write(out.resolve(at + ".asm"), listing, StandardCharsets.UTF_8);
                DecompileResults result = decompiler.decompileFunction(function, 45, monitor);
                Files.write(out.resolve(at + ".c"), (result.decompileCompleted() && result.getDecompiledFunction() != null ?
                    result.getDecompiledFunction().getC() : "// " + result.getErrorMessage()).getBytes(StandardCharsets.UTF_8));
                println("World export " + at + " " + result.decompileCompleted());
            }
        } finally { decompiler.dispose(); }
    }
}
