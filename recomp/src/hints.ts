/** Load discovery hints (function entries, no-return functions, jump-table targets) from JSON. */
import { readFileSync } from "node:fs";
import type { DiscoveryHints } from "./cfg.ts";

interface GhidraExport {
  functions: { entry: number; name: string; thunk: boolean; noreturn: boolean; ranges: [number, number][] }[];
  computedJumps: { at: number; targets: number[] }[];
}

export function loadGhidraHints(path: string): DiscoveryHints & { names: Map<number, string> } {
  const g = JSON.parse(readFileSync(path, "utf8")) as GhidraExport;
  return {
    entries: g.functions.filter((f) => !f.thunk || f.ranges.length > 0).map((f) => f.entry),
    noreturn: g.functions.filter((f) => f.noreturn).map((f) => f.entry),
    jumpTargets: new Map(g.computedJumps.filter((j) => j.targets.length).map((j) => [j.at, j.targets])),
    names: new Map(g.functions.map((f) => [f.entry, f.name])),
  };
}
