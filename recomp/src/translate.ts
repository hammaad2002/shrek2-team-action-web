/**
 * Translates the player's own `Shrek 2.exe` into a WebAssembly module. Runs in the browser (a
 * worker on the player's machine) and in Node (CLI, tests); never ships game code anywhere.
 *
 * Driven by metadata/<sha256>.json: address-level facts about the supported executable (function
 * entry points, no-return functions, jump-table targets) produced offline by `cli.ts metadata`.
 * With a complete entry list no speculative code discovery is needed.
 */
import { Program } from "./cfg.ts";
import { WasmModuleWriter } from "./emit-wasm.ts";
import { liftFunction } from "./lift.ts";
import { PeImage } from "./pe.ts";

export { TRANSLATOR_VERSION } from "./version.ts";

export interface ExeMetadata {
  format: 1;
  exe: { name: string; sha256: string; size: number };
  entries: number[];
  noreturn: number[];
  jumpTargets: [number, number[]][];
}

export interface TranslateResult {
  wasm: Uint8Array;
  functions: number;
  irreducible: number;
  ms: { discover: number; translate: number };
}

export type Progress = (phase: "discover" | "translate" | "encode", done: number, total: number) => void;

const now = () => (typeof performance !== "undefined" ? performance.now() : Date.now());

export function translateExe(exe: Uint8Array, meta: ExeMetadata, progress?: Progress): TranslateResult {
  const t0 = now();
  progress?.("discover", 0, 1);
  const pe = new PeImage(exe);
  const prog = new Program(pe, { entries: meta.entries, noreturn: meta.noreturn, jumpTargets: new Map(meta.jumpTargets) });
  prog.discover({ speculate: false });
  const t1 = now();
  const importIndex = new Map(pe.imports.map((im, i) => [im.iatSlot, i]));
  const entries = [...prog.funcs.keys()].sort((a, b) => a - b);
  const text = pe.sections.find((s) => s.executable)!;
  const writer = new WasmModuleWriter(entries, { textLo: text.va, textHi: text.va + Math.max(text.vsize, text.rawSize) });
  for (let i = 0; i < entries.length; i++) {
    writer.add(liftFunction(prog, prog.funcs.get(entries[i])!, (slot) => importIndex.get(slot)!));
    if (i % 250 === 0) progress?.("translate", i, entries.length);
  }
  progress?.("encode", 0, 1);
  const out = writer.finish();
  return { wasm: out.bytes, functions: entries.length, irreducible: out.irreducible, ms: { discover: t1 - t0, translate: now() - t1 } };
}

/** Offline: record the discovery result for an executable (addresses only, no code bytes). */
export function buildMetadata(prog: Program, exe: { name: string; sha256: string; size: number }): ExeMetadata {
  const jumpTargets: [number, number[]][] = [];
  for (const f of prog.funcs.values())
    for (const b of f.blocks.values()) {
      if (b.term.kind === "switch") jumpTargets.push([b.term.table.at, b.term.table.targets.filter((t) => t >= 0)]);
    }
  jumpTargets.sort((a, b) => a[0] - b[0]);
  return {
    format: 1,
    exe,
    entries: [...prog.funcs.keys()].sort((a, b) => a - b),
    noreturn: [...prog.noreturn].sort((a, b) => a - b),
    jumpTargets,
  };
}
