/**
 * Function discovery and control-flow graphs.
 *
 * Entry sources, strongest first: the PE entry point, direct call targets (recursive), hint
 * metadata (e.g. Ghidra's inventory), RTTI vtables, and speculative code pointers found in data
 * or in instruction immediates. A speculative entry is accepted only if it is not inside an
 * already-discovered function and decodes into a function that does not hit invalid code.
 */
import type { PeImage } from "./pe.ts";
import { FlowControl, Mnemonic, OpKind, Register, X86Decoder, type Insn } from "./x86.ts";

export type Terminator =
  | { kind: "fall"; next: number }
  | { kind: "jmp"; target: number }
  | { kind: "jcc"; target: number; next: number }
  | { kind: "switch"; table: JumpTable }
  | { kind: "ret"; pop: number }
  | { kind: "tail"; target: number } // jmp to another function's entry
  | { kind: "tail-import"; slot: number }
  | { kind: "tail-indirect" } // jmp reg/mem that is not a jump table
  /** Unbounded `jmp [r*4 + table]`: dispatch on the loaded address over code pointers found around the table. */
  | { kind: "switch-addr"; targets: number[] }
  | { kind: "noreturn" } // call to a function that never returns
  | { kind: "trap"; reason: string }; // int3/hlt/ud2/invalid code: abort if reached

export interface Block {
  start: number;
  insns: Insn[];
  term: Terminator;
}

export interface JumpTable {
  at: number;
  /** Address of the dword table. */
  table: number;
  /** Optional byte index table (MSVC `movzx r, byte [r+idx]` form). */
  indexTable: number | null;
  /** Target per index; -1 marks entries the code never takes (they trap). */
  targets: number[];
  bounded: boolean;
}

export interface Func {
  entry: number;
  blocks: Map<number, Block>;
  callees: Set<number>;
  importCalls: Set<number>;
  hasIndirectCalls: boolean;
  speculative: boolean;
  bad: string | null;
}

export interface DiscoveryHints {
  entries?: number[];
  noreturn?: number[];
  /** Jump-table targets by dispatch address (used when bounds cannot be recovered). */
  jumpTargets?: Map<number, number[]>;
}

const NORETURN_IMPORTS = new Set(["kernel32.dll!ExitProcess", "kernel32.dll!ExitThread", "kernel32.dll!FatalExit"]);

/** Instructions that never appear in compiler-generated Win32 user code: data decoded as code. */
const IMPLAUSIBLE = new Set<Mnemonic>([
  Mnemonic.In, Mnemonic.Out, Mnemonic.Insb, Mnemonic.Insw, Mnemonic.Insd, Mnemonic.Outsb, Mnemonic.Outsw, Mnemonic.Outsd,
  Mnemonic.Iret, Mnemonic.Iretd, Mnemonic.Retf, Mnemonic.Into, Mnemonic.Int1, Mnemonic.Int, Mnemonic.Hlt,
  Mnemonic.Les, Mnemonic.Lds, Mnemonic.Lss, Mnemonic.Lfs, Mnemonic.Lgs, Mnemonic.Aam, Mnemonic.Aad,
  Mnemonic.Daa, Mnemonic.Das, Mnemonic.Aaa, Mnemonic.Aas, Mnemonic.Bound, Mnemonic.Arpl, Mnemonic.Sti,
  Mnemonic.Cli, Mnemonic.Salc, Mnemonic.Xlatb, Mnemonic.Enter, Mnemonic.Pushad, Mnemonic.Popad, Mnemonic.Wait,
]);

export class Program {
  readonly pe: PeImage;
  readonly dec: X86Decoder;
  readonly funcs = new Map<number, Func>();
  readonly noreturn = new Set<number>();
  readonly textStart: number;
  readonly textEnd: number;
  readonly vtables = new Map<number, number[]>();
  readonly log: string[] = [];
  private readonly hints: DiscoveryHints;

  constructor(pe: PeImage, hints: DiscoveryHints = {}) {
    this.pe = pe;
    this.hints = hints;
    this.dec = new X86Decoder((va, len) => pe.view(va, len));
    const text = pe.sections.find((s) => s.executable);
    if (!text) throw new Error("no executable section");
    this.textStart = text.va;
    this.textEnd = text.va + Math.max(text.vsize, text.rawSize);
    for (const a of hints.noreturn ?? []) this.noreturn.add(a);
  }

  inText(va: number): boolean {
    return va >= this.textStart && va < this.textEnd;
  }

  /** Discover every function. */
  discover(): void {
    const firm = new Set<number>([this.pe.entry, ...(this.hints.entries ?? [])].filter((a) => this.inText(a)));
    for (const [vt, fns] of this.findVtables()) {
      this.vtables.set(vt, fns);
      for (const f of fns) firm.add(f);
    }
    this.closure([...firm], false);
    // Speculative pointers (data dwords and code immediates), iterated to a fixpoint because
    // newly found functions contribute new immediates. Only targets outside known bodies count.
    const tried = new Set<number>();
    for (let round = 0; round < 16; round++) {
      const covered = this.coveredAddresses();
      const spec = [...this.findCodePointers()].filter((a) => !this.funcs.has(a) && !covered.has(a) && !tried.has(a));
      if (spec.length === 0) break;
      for (const a of spec) tried.add(a);
      this.closure(spec, true);
    }
    this.computeNoReturn();
  }

  /** Decode all functions reachable from the given entries (callees are discovered transitively). */
  private closure(entries: number[], speculative: boolean): void {
    const work = [...entries];
    while (work.length) {
      const e = work.pop()!;
      if (this.funcs.has(e) || !this.inText(e)) continue;
      const f = this.buildFunction(e, speculative);
      if (speculative && f.bad) {
        this.log.push(`rejected speculative entry 0x${e.toString(16)}: ${f.bad}`);
        continue;
      }
      this.funcs.set(e, f);
      for (const c of f.callees) if (!this.funcs.has(c)) work.push(c);
      for (const b of f.blocks.values()) if (b.term.kind === "tail" && !this.funcs.has(b.term.target)) work.push(b.term.target);
    }
  }

  isFunctionEntry(va: number): boolean {
    return this.funcs.has(va) || (this.hints.entries?.includes(va) ?? false);
  }

  private buildFunction(entry: number, speculative: boolean): Func {
    const f: Func = { entry, blocks: new Map(), callees: new Set(), importCalls: new Set(), hasIndirectCalls: false, speculative, bad: null };
    const starts = [entry];
    const knownEntries = (va: number) => va !== entry && this.isFunctionEntry(va);
    while (starts.length) {
      const start = starts.pop()!;
      if (f.blocks.has(start)) continue;
      // Starting inside an existing block: split it.
      const owner = [...f.blocks.values()].find(
        (b) => b.insns.length > 1 && b.start < start && start < b.insns[b.insns.length - 1].next && b.insns.some((i) => i.ip === start),
      );
      if (owner) {
        const k = owner.insns.findIndex((i) => i.ip === start);
        const tail: Block = { start, insns: owner.insns.slice(k), term: owner.term };
        owner.insns = owner.insns.slice(0, k);
        owner.term = { kind: "fall", next: start };
        f.blocks.set(start, tail);
        continue;
      }
      const insns: Insn[] = [];
      let ip = start;
      let term: Terminator | null = null;
      while (!term) {
        if (insns.length > 0 && (f.blocks.has(ip) || starts.includes(ip))) {
          term = { kind: "fall", next: ip };
          break;
        }
        if (insns.length > 0 && knownEntries(ip)) {
          term = { kind: "tail", target: ip }; // falls through into the next function
          break;
        }
        if (!this.inText(ip)) {
          term = { kind: "trap", reason: `ran off .text at 0x${ip.toString(16)}` };
          break;
        }
        const insn = this.dec.decode(ip);
        if (insn.invalid) {
          if (speculative && insns.length === 0) f.bad = `invalid instruction at 0x${ip.toString(16)}`;
          term = { kind: "trap", reason: `invalid instruction at 0x${ip.toString(16)}` };
          if (speculative) f.bad ??= `invalid instruction at 0x${ip.toString(16)}`;
          break;
        }
        if (speculative && IMPLAUSIBLE.has(insn.mnemonic)) {
          f.bad = `implausible ${insn.text} at 0x${ip.toString(16)}`;
          term = { kind: "trap", reason: f.bad };
          break;
        }
        insns.push(insn);
        ip = insn.next;
        term = this.classify(f, insn, insns, starts, knownEntries);
      }
      f.blocks.set(start, { start, insns, term });
    }
    // Overlapping blocks (a block decoded from a mid-instruction address) indicate bad code.
    if (speculative && !f.bad) {
      const owner = new Map<number, number>();
      for (const b of f.blocks.values())
        for (const i of b.insns) {
          for (let a = i.ip; a < i.next; a++) {
            const prev = owner.get(a);
            if (prev !== undefined && prev !== i.ip) {
              f.bad = `overlapping instructions at 0x${a.toString(16)}`;
              break;
            }
            owner.set(a, i.ip);
          }
        }
    }
    return f;
  }

  private classify(f: Func, insn: Insn, blockInsns: Insn[], starts: number[], knownEntry: (va: number) => boolean): Terminator | null {
    const push = (va: number) => {
      if (!starts.includes(va)) starts.push(va);
    };
    switch (insn.flow) {
      case FlowControl.Next:
        if (insn.mnemonic === Mnemonic.Int3 || insn.mnemonic === Mnemonic.Hlt || insn.mnemonic === Mnemonic.Ud2) {
          return { kind: "trap", reason: `${insn.text} at 0x${insn.ip.toString(16)}` };
        }
        return null;
      case FlowControl.Return:
        return { kind: "ret", pop: insn.ops[0]?.kind === "imm" ? insn.ops[0].value : 0 };
      case FlowControl.UnconditionalBranch: {
        if (knownEntry(insn.target) || !this.inText(insn.target)) return { kind: "tail", target: insn.target };
        push(insn.target);
        return { kind: "jmp", target: insn.target };
      }
      case FlowControl.ConditionalBranch: {
        // jcc / loop / jecxz
        if (knownEntry(insn.target)) {
          // Conditional tail call: the target is another function; the lifter emits the call.
          push(insn.next);
          return { kind: "jcc", target: insn.target, next: insn.next };
        }
        push(insn.target);
        push(insn.next);
        return { kind: "jcc", target: insn.target, next: insn.next };
      }
      case FlowControl.Call: {
        if (insn.target === insn.next) return null; // call $+5 (get-EIP idiom)
        if (this.inText(insn.target)) {
          f.callees.add(insn.target);
          if (this.noreturn.has(insn.target)) return { kind: "noreturn" };
        }
        return null;
      }
      case FlowControl.IndirectCall: {
        const slot = this.iatSlotOf(insn);
        if (slot !== null) {
          f.importCalls.add(slot);
          const imp = this.pe.importBySlot.get(slot)!;
          if (NORETURN_IMPORTS.has(`${imp.dll}!${imp.name}`)) return { kind: "noreturn" };
        } else f.hasIndirectCalls = true;
        return null;
      }
      case FlowControl.IndirectBranch: {
        const slot = this.iatSlotOf(insn);
        if (slot !== null) return { kind: "tail-import", slot };
        const jt = this.resolveJumpTable(insn, blockInsns, f);
        if (jt) {
          for (const t of jt.targets) if (t >= 0) push(t);
          return { kind: "switch", table: jt };
        }
        const cands = f.speculative ? [] : this.tableCandidates(insn);
        if (cands.length) {
          for (const t of cands) push(t);
          this.log.push(`jump table at 0x${insn.ip.toString(16)}: address switch over ${cands.length} candidate(s)`);
          return { kind: "switch-addr", targets: cands };
        }
        return { kind: "tail-indirect" };
      }
      case FlowControl.Interrupt:
      case FlowControl.Exception:
        return { kind: "trap", reason: `${insn.text} at 0x${insn.ip.toString(16)}` };
      default:
        return { kind: "trap", reason: `unsupported flow ${FlowControl[insn.flow]} at 0x${insn.ip.toString(16)}` };
    }
  }

  /** `call/jmp dword ptr [abs]` where abs is an IAT slot. */
  iatSlotOf(insn: Insn): number | null {
    const m = insn.mem;
    if (!m || m.base !== Register.None || m.index !== Register.None) return null;
    return this.pe.importBySlot.has(m.disp) ? m.disp : null;
  }

  /** Recover an MSVC switch table at `jmp dword ptr [r*4 + table]`. */
  private resolveJumpTable(insn: Insn, blockInsns: Insn[], f: Func): JumpTable | null {
    const m = insn.mem;
    if (!m || m.scale !== 4 || m.index === Register.None || m.base !== Register.None || m.size !== 4) return null;
    const table = m.disp;
    if (!this.pe.contains(table)) return null;
    // Look back over this block and its fall-through predecessors for the bounds check.
    const history = [...this.linearHistory(f, blockInsns)];
    let idxReg = m.index;
    let indexTable: number | null = null;
    let count: number | null = null;
    let masked = false;
    for (let k = history.length - 2; k >= 0 && k >= history.length - 12; k--) {
      const h = history[k];
      // movzx idxReg, byte ptr [r + idxtable]
      if (indexTable === null && h.mnemonic === Mnemonic.Movzx && h.ops[0]?.kind === "reg" && sameReg(h.ops[0].reg, idxReg) && h.mem && h.mem.size === 1 && h.mem.index === Register.None && h.mem.base !== Register.None) {
        indexTable = h.mem.disp;
        idxReg = h.mem.base;
        continue;
      }
      if (h.mnemonic === Mnemonic.Cmp && h.ops[0]?.kind === "reg" && sameReg(h.ops[0].reg, idxReg) && h.ops[1]?.kind === "imm") {
        count = h.ops[1].value + 1;
        break;
      }
      // `and idx, mask` (e.g. CRT memcpy tail dispatch) bounds the index to [0, mask].
      if (h.mnemonic === Mnemonic.And && h.ops[0]?.kind === "reg" && sameReg(h.ops[0].reg, idxReg) && h.ops[1]?.kind === "imm" && h.ops[1].value < 256) {
        count = h.ops[1].value + 1;
        masked = true;
        break;
      }
      // Any other write to the index register before a bound was found: give up on this chain.
      if (h.ops[0]?.kind === "reg" && sameReg(h.ops[0].reg, idxReg) && h.mnemonic !== Mnemonic.Cmp && h.mnemonic !== Mnemonic.Test) break;
    }
    const hinted = this.hints.jumpTargets?.get(insn.ip);
    let targets: number[] = [];
    let bounded = false;
    if (count !== null && count > 0 && count < 4096) {
      if (indexTable !== null) {
        let max = 0;
        for (let i = 0; i < count; i++) max = Math.max(max, this.pe.u8(indexTable + i));
        for (let i = 0; i <= max; i++) targets.push(this.pe.u32(table + i * 4));
      } else {
        for (let i = 0; i < count; i++) targets.push(this.pe.u32(table + i * 4));
      }
      bounded = targets.every((t) => this.inText(t));
      // A mask only bounds the index; the code may never use some values (CRT memcpy's LeadUpVec
      // has a garbage entry 0 that overlaps the preceding instruction). Unused entries trap.
      if (!bounded && masked && targets.some((t) => this.inText(t))) {
        targets = targets.map((t) => (this.inText(t) ? t : -1));
        bounded = true;
        this.log.push(`jump table at 0x${insn.ip.toString(16)}: masked, invalid entries trap`);
      }
    }
    if (!bounded) {
      if (hinted && hinted.length) {
        // Rebuild the table from the dword entries, keeping the hinted target set authoritative.
        targets = [];
        for (let i = 0; i < 4096; i++) {
          const t = this.pe.u32(table + i * 4);
          if (!hinted.includes(t)) break;
          targets.push(t);
        }
        if (targets.length === 0) return null;
        this.log.push(`jump table at 0x${insn.ip.toString(16)}: bounds from hints (${targets.length})`);
      } else {
        this.log.push(`jump table at 0x${insn.ip.toString(16)}: unbounded`);
        return null;
      }
    }
    return { at: insn.ip, table, indexTable, targets, bounded };
  }

  /**
   * Code pointers near the table of an unbounded `jmp [r*4 + table]` (indices may even be negative,
   * as in CRT memcpy's TrailUpVec). Entries must point close to the jump itself; scanning stops in
   * each direction after two consecutive misses.
   */
  private tableCandidates(insn: Insn): number[] {
    const m = insn.mem;
    if (!m || m.scale !== 4 || m.index === Register.None || m.base !== Register.None || m.size !== 4) return [];
    const table = m.disp;
    const ok = (t: number) => this.inText(t) && Math.abs(t - insn.ip) < 0x1000;
    const out = new Set<number>();
    for (const dir of [1, -1]) {
      let misses = 0;
      for (let i = dir === 1 ? 0 : -1; Math.abs(i) < 64 && misses < 2; i += dir) {
        const a = table + 4 * i;
        if (!this.pe.contains(a)) break;
        const t = this.pe.u32(a);
        if (ok(t)) {
          out.add(t);
          misses = 0;
        } else misses++;
      }
    }
    return [...out].sort((a, b) => a - b);
  }

  /** Instructions on the fall-through path ending at the current position (most recent last). */
  private *linearHistory(f: Func, blockInsns: Insn[]): Generator<Insn> {
    const chain: Insn[][] = [blockInsns];
    let first = blockInsns[0];
    for (let depth = 0; depth < 3 && first; depth++) {
      const pred = [...f.blocks.values()].find(
        (b) => (b.term.kind === "fall" && b.term.next === first!.ip) || (b.term.kind === "jcc" && b.term.next === first!.ip),
      );
      if (!pred || pred.insns.length === 0) break;
      chain.unshift(pred.insns);
      first = pred.insns[0];
    }
    for (const c of chain) yield* c;
  }

  /** RTTI-backed vtables: [COL*][fn][fn]... in .rdata. */
  private findVtables(): Map<number, number[]> {
    const out = new Map<number, number[]>();
    const rdata = this.pe.sections.filter((s) => !s.executable && !s.writable);
    for (const s of rdata) {
      for (let a = s.va; a + 8 <= s.va + s.vsize; a += 4) {
        const col = this.pe.u32(a);
        if (!this.isCol(col)) continue;
        const fns: number[] = [];
        for (let p = a + 4; p < s.va + s.vsize; p += 4) {
          const fn = this.pe.u32(p);
          if (!this.inText(fn)) break;
          fns.push(fn);
        }
        if (fns.length) out.set(a + 4, fns);
      }
    }
    return out;
  }

  private isCol(va: number): boolean {
    if (!this.pe.contains(va) || va & 3) return false;
    const s = this.pe.section(va);
    if (!s || s.executable) return false;
    if (this.pe.u32(va) !== 0) return false; // signature
    const td = this.pe.u32(va + 12);
    if (!this.pe.contains(td) || td & 3) return false;
    const name = td + 8;
    return this.pe.u8(name) === 0x2e && this.pe.u8(name + 1) === 0x3f && this.pe.u8(name + 2) === 0x41; // ".?A"
  }

  /** Aligned dwords in data sections and imm32 operands in code that point into .text. */
  private findCodePointers(): Set<number> {
    const out = new Set<number>();
    for (const s of this.pe.sections) {
      if (s.executable) continue;
      const end = s.va + Math.min(s.vsize, s.rawSize);
      for (let a = s.va; a + 4 <= end; a += 4) {
        const v = this.pe.u32(a);
        if (this.inText(v)) out.add(v);
      }
    }
    for (const f of this.funcs.values())
      for (const b of f.blocks.values())
        for (const i of b.insns)
          for (const op of i.ops) if (op.kind === "imm" && this.inText(op.value) && i.mnemonic !== Mnemonic.Cmp) out.add(op.value);
    return out;
  }

  private coveredAddresses(): Set<number> {
    const covered = new Set<number>();
    for (const f of this.funcs.values())
      for (const b of f.blocks.values())
        for (const i of b.insns) for (let a = i.ip; a < i.next; a++) if (a !== f.entry) covered.add(a);
    return covered;
  }

  /** Fixpoint: a function never returns if no block returns and no tail call reaches a returning function. */
  private computeNoReturn(): void {
    let changed = true;
    while (changed) {
      changed = false;
      for (const f of this.funcs.values()) {
        if (this.noreturn.has(f.entry)) continue;
        let returns = false;
        for (const b of f.blocks.values()) {
          const t = b.term;
          if (t.kind === "ret" || t.kind === "tail-import" || t.kind === "tail-indirect" || t.kind === "switch-addr") returns = true;
          else if (t.kind === "tail" && !this.noreturn.has(t.target)) returns = true;
          else if (t.kind === "jcc" && this.isFunctionEntry(t.target) && !this.noreturn.has(t.target)) returns = true;
        }
        if (!returns) {
          this.noreturn.add(f.entry);
          changed = true;
        }
      }
    }
  }
}

/** True if two registers are the same architectural GPR (ignoring 8/16/32-bit width). */
export function sameReg(a: Register, b: Register): boolean {
  return gprIndex(a) !== -1 && gprIndex(a) === gprIndex(b);
}

const GPR32 = [Register.EAX, Register.ECX, Register.EDX, Register.EBX, Register.ESP, Register.EBP, Register.ESI, Register.EDI];
const GPR16 = [Register.AX, Register.CX, Register.DX, Register.BX, Register.SP, Register.BP, Register.SI, Register.DI];
const GPR8L = [Register.AL, Register.CL, Register.DL, Register.BL];
const GPR8H = [Register.AH, Register.CH, Register.DH, Register.BH];

export function gprIndex(r: Register): number {
  let k = GPR32.indexOf(r);
  if (k >= 0) return k;
  k = GPR16.indexOf(r);
  if (k >= 0) return k;
  k = GPR8L.indexOf(r);
  if (k >= 0) return k;
  return GPR8H.indexOf(r);
}

export { OpKind };
