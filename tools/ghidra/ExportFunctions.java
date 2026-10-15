// Exports function boundaries, no-return flags and computed-jump targets as JSON.
// Usage (headless): -postScript ExportFunctions.java <out.json>
// @category Shrek2Web
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
  @Override
  public void run() throws Exception {
    String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "functions.json";
    try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
      w.println("{\"functions\":[");
      FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
      boolean first = true;
      while (it.hasNext()) {
        Function f = it.next();
        if (!first) w.println(",");
        first = false;
        w.print(String.format("{\"entry\":%d,\"name\":\"%s\",\"thunk\":%b,\"noreturn\":%b,\"ranges\":[",
            f.getEntryPoint().getOffset(), esc(f.getName()), f.isThunk(), f.hasNoReturn()));
        boolean fr = true;
        for (AddressRange r : f.getBody()) {
          if (!fr) w.print(",");
          fr = false;
          w.print("[" + r.getMinAddress().getOffset() + "," + r.getMaxAddress().getOffset() + "]");
        }
        w.print("]}");
      }
      w.println("],\"computedJumps\":[");
      InstructionIterator ii = currentProgram.getListing().getInstructions(true);
      first = true;
      while (ii.hasNext()) {
        Instruction ins = ii.next();
        if (!(ins.getFlowType().isComputed() && ins.getFlowType().isJump())) continue;
        if (!first) w.println(",");
        first = false;
        w.print("{\"at\":" + ins.getAddress().getOffset() + ",\"targets\":[");
        boolean ft = true;
        for (Reference ref : ins.getReferencesFrom()) {
          if (!ref.getReferenceType().isFlow()) continue;
          if (!ft) w.print(",");
          ft = false;
          w.print(ref.getToAddress().getOffset());
        }
        w.print("]}");
      }
      w.println("]}");
    }
  }

  private static String esc(String s) {
    return s.replace("\\", "\\\\").replace("\"", "\\\"");
  }
}
