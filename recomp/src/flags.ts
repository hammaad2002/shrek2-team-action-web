/**
 * Static EFLAGS analysis for one function.
 *
 * Arithmetic flags are only computed where something reads them. For every flag consumer we
 * look at the reaching definitions of each flag it reads:
 *  - "snapshot": all reaching definitions are the same kind of instruction (e.g. `cmp`) and
 *    define every flag the consumer needs, so the consumer evaluates its condition directly from
 *    the defining instruction's saved operands (`cmp a,b; jl` -> `(int)a < (int)b`);
 *  - "materialize": otherwise each reaching definition writes the needed flag bits into flag
 *    locals and the consumer reads those.
 * Flags are assumed dead across calls and at function exit (MSVC ABI); reads of flags reaching
 * from the function entry or a call are reported as `unknownReads`.
 */
import type { Block, Func } from "./cfg.ts";
import { Mnemonic, OpKind, Register, type Insn } from "./x86.ts";

export const CF = 1;
export const PF = 2;
export const AF = 4;
export const ZF = 8;
export const SF = 16;
export const OF = 32;
export const ALL_FLAGS = 63;
export const FLAG_NAMES: [number, string][] = [[CF, "cf"], [PF, "pf"], [AF, "af"], [ZF, "zf"], [SF, "sf"], [OF, "of"]];

/** Convert iced RflagsBits (OF=1 SF=2 ZF=4 AF=8 CF=16 PF=32) to our bit order. */
export function fromIced(m: number): number {
  let r = 0;
  if (m & 1) r |= OF;
  if (m & 2) r |= SF;
  if (m & 4) r |= ZF;
  if (m & 8) r |= AF;
  if (m & 16) r |= CF;
  if (m & 32) r |= PF;
  return r;
}

export type DefKind = "add" | "sub" | "logic" | "inc" | "dec" | "neg" | "other";

export type UseMode = { mode: "snapshot"; kind: DefKind; size: number } | { mode: "materialize" };

export interface FlagPlan {
  liveAfter: Map<number, number>;
  snapshot: Set<number>;
  materialize: Map<number, number>;
  useMode: Map<number, UseMode>;
  unknownReads: number[];
}

/** Definition sites: instruction ip, or UNKNOWN (function entry / after a call). */
const UNKNOWN = -1;

export function defKind(insn: Insn): { kind: DefKind; size: number } {
  const size = operandBits(insn);
  switch (insn.mnemonic) {
    case Mnemonic.Add:
    case Mnemonic.Xadd:
      return { kind: "add", size };
    case Mnemonic.Sub:
    case Mnemonic.Cmp:
    case Mnemonic.Cmpxchg:
      return { kind: "sub", size };
    // Repeated string compares leave flags untouched when ECX = 0: not a snapshot kind.
    case Mnemonic.Cmpsb:
    case Mnemonic.Scasb:
      return { kind: insn.rep || insn.repne ? "other" : "sub", size: 8 };
    case Mnemonic.Cmpsw:
    case Mnemonic.Scasw:
      return { kind: insn.rep || insn.repne ? "other" : "sub", size: 16 };
    case Mnemonic.Cmpsd:
    case Mnemonic.Scasd:
      if (insn.ops.some((o) => o.kind === "reg" && isXmm(o.reg))) return { kind: "other", size };
      return { kind: insn.rep || insn.repne ? "other" : "sub", size: 32 };
    case Mnemonic.And:
    case Mnemonic.Or:
    case Mnemonic.Xor:
    case Mnemonic.Test:
      return { kind: "logic", size };
    case Mnemonic.Inc:
      return { kind: "inc", size };
    case Mnemonic.Dec:
      return { kind: "dec", size };
    case Mnemonic.Neg:
      return { kind: "neg", size };
    default:
      return { kind: "other", size };
  }
}

/** Flags each snapshot kind can reproduce from its saved operands. */
const SNAPSHOT_FLAGS: Record<DefKind, number> = {
  add: ALL_FLAGS,
  sub: ALL_FLAGS,
  logic: ALL_FLAGS,
  inc: ALL_FLAGS & ~CF,
  dec: ALL_FLAGS & ~CF,
  neg: ALL_FLAGS,
  other: 0,
};

export function isCall(insn: Insn): boolean {
  return insn.mnemonic === Mnemonic.Call;
}

/** Flags read and written by an instruction (our bit order), with ABI adjustments. */
export function useDef(insn: Insn): { use: number; def: number } {
  if (isCall(insn)) return { use: 0, def: ALL_FLAGS };
  let use = fromIced(insn.rflagsRead);
  const def = fromIced(insn.rflagsModified);
  // Shifts/rotates by CL leave flags untouched when the count is zero: they read what they write.
  if (isShiftLike(insn.mnemonic) && insn.ops[1]?.kind === "reg") use |= def;
  if ((insn.rep || insn.repne) && isStringCompare(insn.mnemonic)) use |= def;
  if (insn.mnemonic === Mnemonic.Pushfd || insn.mnemonic === Mnemonic.Pushf) use |= ALL_FLAGS;
  if (insn.mnemonic === Mnemonic.Lahf) use |= ALL_FLAGS & ~OF;
  return { use, def };
}

export function isStringCompare(m: Mnemonic): boolean {
  return m === Mnemonic.Cmpsb || m === Mnemonic.Cmpsw || m === Mnemonic.Cmpsd || m === Mnemonic.Scasb || m === Mnemonic.Scasw || m === Mnemonic.Scasd;
}

export function isShiftLike(m: Mnemonic): boolean {
  return (
    m === Mnemonic.Shl || m === Mnemonic.Sal || m === Mnemonic.Shr || m === Mnemonic.Sar || m === Mnemonic.Rol || m === Mnemonic.Ror ||
    m === Mnemonic.Rcl || m === Mnemonic.Rcr || m === Mnemonic.Shld || m === Mnemonic.Shrd
  );
}

export function successors(f: Func, b: Block): number[] {
  const t = b.term;
  switch (t.kind) {
    case "fall":
      return [t.next];
    case "jmp":
      return [t.target];
    case "jcc":
      return f.blocks.has(t.target) ? [t.target, t.next] : [t.next];
    case "switch":
      return [...new Set(t.table.targets)];
    default:
      return [];
  }
}

export function analyzeFlags(f: Func): FlagPlan {
  const blocks = [...f.blocks.values()];
  const ud = new Map<number, { use: number; def: number }>();
  for (const b of blocks) for (const i of b.insns) ud.set(i.ip, useDef(i));

  // ---- Liveness (backward). Flags are dead at returns, tail calls and traps.
  const liveIn = new Map<number, number>(blocks.map((b) => [b.start, 0]));
  const liveAfter = new Map<number, number>();
  let changed = true;
  while (changed) {
    changed = false;
    for (let bi = blocks.length - 1; bi >= 0; bi--) {
      const b = blocks[bi];
      let live = 0;
      for (const s of successors(f, b)) live |= liveIn.get(s) ?? 0;
      // A conditional tail call reads flags in the terminator itself (the jcc instruction).
      for (let k = b.insns.length - 1; k >= 0; k--) {
        const i = b.insns[k];
        liveAfter.set(i.ip, live);
        const { use, def } = ud.get(i.ip)!;
        live = (live & ~def) | use;
      }
      if (live !== liveIn.get(b.start)) {
        liveIn.set(b.start, live);
        changed = true;
      }
    }
  }

  // ---- Reaching definitions per flag (forward).
  type Defs = Set<number>[]; // index by flag bit position 0..5
  const bitIndex = (flag: number) => Math.log2(flag);
  const emptyDefs = (): Defs => Array.from({ length: 6 }, () => new Set<number>());
  const inDefs = new Map<number, Defs>();
  for (const b of blocks) inDefs.set(b.start, emptyDefs());
  const entryDefs = inDefs.get(f.entry);
  if (entryDefs) for (const s of entryDefs) s.add(UNKNOWN);
  const usesAt = new Map<number, Defs>(); // reaching defs at each use instruction
  changed = true;
  while (changed) {
    changed = false;
    for (const b of blocks) {
      const cur = inDefs.get(b.start)!.map((s) => new Set(s));
      for (const i of b.insns) {
        const { use, def } = ud.get(i.ip)!;
        if (use) usesAt.set(i.ip, cur.map((s) => new Set(s)));
        for (let k = 0; k < 6; k++) {
          if (def & (1 << k)) {
            cur[k].clear();
            cur[k].add(isCall(i) ? UNKNOWN : i.ip);
          }
        }
      }
      for (const s of successors(f, b)) {
        const target = inDefs.get(s);
        if (!target) continue;
        for (let k = 0; k < 6; k++)
          for (const d of cur[k])
            if (!target[k].has(d)) {
              target[k].add(d);
              changed = true;
            }
      }
    }
  }

  // ---- Strategy per use.
  const byIp = new Map<number, Insn>();
  for (const b of blocks) for (const i of b.insns) byIp.set(i.ip, i);
  const plan: FlagPlan = { liveAfter, snapshot: new Set(), materialize: new Map(), useMode: new Map(), unknownReads: [] };
  for (const [ip, defs] of usesAt) {
    const { use } = ud.get(ip)!;
    const needed = FLAG_NAMES.map(([fl]) => fl).filter((fl) => use & fl);
    const all = new Set<number>();
    let unknown = false;
    for (const fl of needed) for (const d of defs[bitIndex(fl)]) d === UNKNOWN ? (unknown = true) : all.add(d);
    if (unknown) plan.unknownReads.push(ip);
    let mode: UseMode = { mode: "materialize" };
    if (!unknown && all.size > 0) {
      const kinds = [...all].map((d) => defKind(byIp.get(d)!));
      const k0 = kinds[0];
      const sameKind = kinds.every((k) => k.kind === k0.kind && k.size === k0.size) && k0.kind !== "other";
      const coversAll = needed.every((fl) => (SNAPSHOT_FLAGS[k0.kind] & fl) !== 0 && defs[bitIndex(fl)].size === all.size && [...all].every((d) => defs[bitIndex(fl)].has(d)));
      if (sameKind && coversAll) mode = { mode: "snapshot", kind: k0.kind, size: k0.size };
    }
    plan.useMode.set(ip, mode);
    if (mode.mode === "snapshot") for (const d of all) plan.snapshot.add(d);
    else
      for (const fl of needed)
        for (const d of defs[bitIndex(fl)]) if (d !== UNKNOWN) plan.materialize.set(d, (plan.materialize.get(d) ?? 0) | fl);
  }
  return plan;
}

export function operandBits(insn: Insn): number {
  const o = insn.ops[0];
  if (!o) return 32;
  if (o.kind === "reg") return regBits(o.reg);
  if (o.kind === "mem") return o.mem.size * 8 || 32;
  return 32;
}

export function regBits(r: Register): number {
  if (r >= Register.AL && r <= Register.R15L) return 8;
  if (r >= Register.AX && r <= Register.R15W) return 16;
  if (r >= Register.EAX && r <= Register.R15D) return 32;
  return 32;
}

export function isXmm(r: Register): boolean {
  return r >= Register.XMM0 && r <= Register.XMM31;
}

export { OpKind };
