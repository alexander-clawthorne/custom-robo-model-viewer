// Decompile functions at the given addresses into one C file (headless).
// args: <output.c> <addr> [addr ...]
// Run through tools/ghidra/decompile.ps1.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionContaining(a);
                if (f == null) {
                    disassemble(a);
                    f = createFunction(a, null);
                }
                if (f == null) {
                    out.println("// " + args[i] + ": no function");
                    continue;
                }
                DecompileResults r = ifc.decompileFunction(f, 120, monitor);
                out.println("// ---- " + f.getName() + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
        ifc.dispose();
    }
}
