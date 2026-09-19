// Correct a false no-return annotation on J460's shared logging routine.
// @category Isaac

import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.SourceType;
import java.util.*;

public class FixJ460Logger extends GhidraScript {
    @Override public void run() throws Exception {
        if (!"3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b".equalsIgnoreCase(currentProgram.getExecutableSHA256()))
            throw new IllegalArgumentException("Expected the fingerprint-checked J460 analysis sample");
        Address entry = currentProgram.getImageBase().add(0x6112c0);
        Function logger = getFunctionAt(entry);
        if (logger == null) throw new IllegalStateException("Logging function was not found");
        // The recorded session continues past calls that print ordinary informational logs.
        // Therefore this shared routine is not unconditionally no-return.
        logger.setNoReturn(false);
        // Provisional cdecl varargs shape, inferred from the level/format call sites.
        // Preserve an unknown return value rather than asserting void.
        logger.setCallingConvention("__cdecl");
        logger.setReturnType(Undefined4DataType.dataType, SourceType.USER_DEFINED);
        logger.replaceParameters(Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true,
            SourceType.USER_DEFINED,
            new ParameterImpl("level", IntegerDataType.dataType, currentProgram),
            new ParameterImpl("format", new PointerDataType(CharDataType.dataType), currentProgram));
        logger.setVarArgs(true);
        logger.setSignatureSource(SourceType.USER_DEFINED);
        logger.setName("IsaacLog_J460_candidate", SourceType.USER_DEFINED);
        List<Address> calls = new ArrayList<>();
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(entry);
        while (refs.hasNext()) {
            Reference ref = refs.next();
            if (ref.getReferenceType().isCall()) calls.add(ref.getFromAddress());
        }
        Set<Function> changed = new HashSet<>();
        int corrected = 0;
        for (Address call : calls) {
            Instruction instruction = getInstructionAt(call);
            if (instruction == null) continue;
            if (instruction.getFlowOverride() == FlowOverride.CALL_RETURN) {
                Function caller = getFunctionContaining(call);
                instruction.setFlowOverride(FlowOverride.NONE);
                disassemble(instruction.getMaxAddress().add(1));
                if (caller != null) changed.add(caller);
                corrected++;
            }
        }
        for (Function fn : changed) CreateFunctionCmd.fixupFunctionBody(currentProgram, fn, monitor);
        println("Cleared " + corrected + " logger call no-return overrides; repaired " + changed.size() + " caller bodies");
    }
}
