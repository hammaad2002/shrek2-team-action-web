/**
 * Developer CLI: discover functions and report coverage statistics.
 *   node recomp/src/analyze.ts <exe> [--ghidra research/ghidra-functions.json] [--no-hints]
 */
import { readFileSync } from "node:fs";
import { Program } from "./cfg.ts";
import { loadGhidraHints } from "./hints.ts";
import { PeImage } from "./pe.ts";
import { Mnemonic } from "./x86.ts";

const args = process.argv.slice(2);
const exe = args[0];
const ghidraPath = args.includes("--ghidra") ? args[args.indexOf("--ghidra") + 1] : undefined;
const pe = new PeImage(new Uint8Array(readFileSync(exe)));
const ghidra = ghidraPath ? loadGhidraHints(ghidraPath) : undefined;
const useHints = ghidra && !args.includes("--no-hints");
const t0 = performance.now();
const prog = new Program(pe, useHints ? ghidra : { noreturn: ghidra?.noreturn });
prog.discover();
const ms = performance.now() - t0;

let blocks = 0, insns = 0, traps = 0, switches = 0, tails = 0, tailInd = 0, spec = 0, bad = 0;
const mnem = new Map<string, number>();
for (const f of prog.funcs.values()) {
  if (f.speculative) spec++;
  if (f.bad) bad++;
  for (const b of f.blocks.values()) {
    blocks++;
    insns += b.insns.length;
    if (b.term.kind === "trap") traps++;
    if (b.term.kind === "switch") switches++;
    if (b.term.kind === "tail") tails++;
    if (b.term.kind === "tail-indirect") tailInd++;
    for (const i of b.insns) {
      const k = Mnemonic[i.mnemonic];
      mnem.set(k, (mnem.get(k) ?? 0) + 1);
    }
  }
}
console.log(`discovered ${prog.funcs.size} functions (${spec} speculative, ${bad} bad) in ${ms.toFixed(0)} ms`);
console.log(`blocks ${blocks}, instructions ${insns} (unique decoded ${prog.dec.size}), switch tables ${switches}, tail calls ${tails}, indirect tail jumps ${tailInd}, trap blocks ${traps}`);
console.log(`vtables ${prog.vtables.size}, no-return functions ${prog.noreturn.size}`);
if (ghidra) {
  const gset = new Set(ghidra.entries);
  const ours = new Set(prog.funcs.keys());
  const missing = [...gset].filter((a) => !ours.has(a));
  const extra = [...ours].filter((a) => !gset.has(a));
  console.log(`vs Ghidra: ${gset.size} ghidra entries; missing ${missing.length}; extra ${extra.length}`);
  console.log(`  missing sample: ${missing.slice(0, 12).map((a) => `0x${a.toString(16)}:${ghidra.names.get(a)}`).join(" ")}`);
  console.log(`  extra sample: ${extra.slice(0, 12).map((a) => "0x" + a.toString(16)).join(" ")}`);
}
console.log(`log (${prog.log.length}): ${prog.log.filter((l) => !l.startsWith("rejected")).slice(0, 20).join("\n  ")}`);
console.log(`rejected speculative: ${prog.log.filter((l) => l.startsWith("rejected")).length}`);
const sorted = [...mnem.entries()].sort((a, b) => b[1] - a[1]);
console.log(`${sorted.length} distinct mnemonics:\n` + sorted.map(([k, v]) => `${k}:${v}`).join(" "));
