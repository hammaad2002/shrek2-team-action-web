/** Discovery hints (function entries, no-return functions, jump-table targets). */
import type { DiscoveryHints } from "./cfg.ts";

interface GhidraExport {
  functions: { entry: number; name: string; thunk: boolean; noreturn: boolean; ranges: [number, number][] }[];
  computedJumps: { at: number; targets: number[] }[];
}

/** Hints from the Ghidra export (research/ghidra-functions.json, local only). */
export function parseGhidraHints(json: string): DiscoveryHints & { names: Map<number, string> } {
  const g = JSON.parse(json) as GhidraExport;
  return {
    entries: g.functions.filter((f) => !f.thunk || f.ranges.length > 0).map((f) => f.entry),
    noreturn: g.functions.filter((f) => f.noreturn).map((f) => f.entry),
    jumpTargets: new Map(g.computedJumps.filter((j) => j.targets.length).map((j) => [j.at, j.targets])),
    names: new Map(g.functions.map((f) => [f.entry, f.name])),
  };
}

export async function loadGhidraHints(path: string): Promise<DiscoveryHints & { names: Map<number, string> }> {
  const { readFileSync } = await import("node:fs");
  return parseGhidraHints(readFileSync(path, "utf8"));
}
