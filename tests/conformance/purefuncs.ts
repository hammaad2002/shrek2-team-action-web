/**
 * Lists game functions whose whole call tree avoids imports and indirect control flow, so they
 * can be executed identically in Unicorn and in recompiled code.
 *   node tests/conformance/purefuncs.ts <game.exe> <ghidra-hints.json> <out.json>
 */
import { readFileSync, writeFileSync } from "node:fs";
import { Program } from "../../recomp/src/cfg.ts";
import { loadGhidraHints } from "../../recomp/src/hints.ts";
import { PeImage } from "../../recomp/src/pe.ts";
import { Mnemonic, Register } from "../../recomp/src/x86.ts";

const [exePath, hintsPath, outPath] = process.argv.slice(2);
const pe = new PeImage(new Uint8Array(readFileSync(exePath)));
const prog = new Program(pe, await loadGhidraHints(hintsPath));
prog.discover();

const localOk = new Map<number, boolean>();
for (const f of prog.funcs.values()) {
  let ok = !f.hasIndirectCalls && f.importCalls.size === 0 && !f.bad;
  for (const b of f.blocks.values()) {
    const t = b.term.kind;
    if (t === "tail-import" || t === "tail-indirect" || t === "trap" || t === "noreturn") ok = false;
    for (const i of b.insns) {
      if (i.mnemonic === Mnemonic.Cpuid || i.mnemonic === Mnemonic.Int3) ok = false;
      if (i.ops.some((o) => o.kind === "reg" && ((o.reg >= Register.MM0 && o.reg <= Register.MM7) || (o.reg >= Register.XMM0 && o.reg <= Register.XMM7)))) ok = false;
    }
  }
  localOk.set(f.entry, ok);
}
const memo = new Map<number, boolean>();
const pure = (e: number, stack: Set<number>): boolean => {
  if (memo.has(e)) return memo.get(e)!;
  const f = prog.funcs.get(e);
  if (!f || !localOk.get(e)) return false;
  if (stack.has(e)) return true;
  stack.add(e);
  let ok = true;
  for (const c of f.callees) if (!pure(c, stack)) ok = false;
  for (const b of f.blocks.values()) if (b.term.kind === "tail" && !pure(b.term.target, stack)) ok = false;
  stack.delete(e);
  memo.set(e, ok);
  return ok;
};
const list = [...prog.funcs.keys()].filter((e) => !prog.funcs.get(e)!.speculative && pure(e, new Set())).sort((a, b) => a - b);
const insnCount = (e: number) => [...prog.funcs.get(e)!.blocks.values()].reduce((n, b) => n + b.insns.length, 0);
writeFileSync(outPath, JSON.stringify(list.map((e) => ({ addr: e, insns: insnCount(e) }))));
console.log(`${list.length} self-contained functions of ${prog.funcs.size}`);
