/**
 * x86-32 -> IR lifter for one discovered function.
 *
 * Conventions:
 *  - Guest GPRs are locals (`eax`..`edi`), loaded from the Cpu struct at entry and after every
 *    call, stored back before calls, returns and tail calls ("syncout"/"syncin").
 *  - The guest stack and return addresses live in guest memory exactly as on real hardware.
 *  - The x87 register file stays in the Cpu struct (`fpr[8]`), indexed by the `ftop` local.
 *  - Arithmetic flags follow the static plan from flags.ts.
 */
import type { Block, Func, Program } from "./cfg.ts";
import { gprIndex } from "./cfg.ts";
import {
  ALL_FLAGS, AF, CF, OF, PF, SF, ZF, FLAG_NAMES, analyzeFlags, defKind, fromIced, operandBits, regBits,
  type DefKind, type FlagPlan,
} from "./flags.ts";
import {
  add, and, bin, c32, c64, call, cf64, cpu, eq, eqz, load, loc, ne, or, sel, shl, shrs, shru, sub, un, xor,
  type CpuField, type Expr, type IrBlock, type IrFunc, type Stmt, type Term, type Ty,
} from "./ir.ts";
import { ConditionCode, MemorySize, Mnemonic, Register, type Insn, type MemOperand, type Operand } from "./x86.ts";

export const GPR_NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"] as const;

const hex = (n: number) => n.toString(16).padStart(8, "0");
export const funcName = (va: number) => `f_${hex(va)}`;
const label = (va: number) => `b_${hex(va)}`;

const MASK = (bits: number) => (bits === 32 ? -1 : (1 << bits) - 1);
const SIGN = (bits: number) => (bits === 32 ? 0x80000000 | 0 : 1 << (bits - 1));

export class LiftError extends Error {}

export function liftFunction(prog: Program, f: Func, importIndex: (slot: number) => number): IrFunc {
  return new FnLifter(prog, f, importIndex).lift();
}

class FnLifter {
  private readonly prog: Program;
  private readonly f: Func;
  private readonly plan: FlagPlan;
  private readonly importIndex: (slot: number) => number;
  private readonly ir: IrFunc;
  private cur!: IrBlock;
  private tmpN = 0;
  private synthN = 0;
  /** Flags read by the instruction currently being lifted (only these may be evaluated). */
  private insn!: Insn;

  constructor(prog: Program, f: Func, importIndex: (slot: number) => number) {
    this.prog = prog;
    this.f = f;
    this.importIndex = importIndex;
    this.plan = analyzeFlags(f);
    this.ir = { entry: f.entry, name: funcName(f.entry), locals: new Map(), order: [], blocks: new Map() };
    for (const g of GPR_NAMES) this.ir.locals.set(g, "i32");
    for (const [, n] of FLAG_NAMES) this.ir.locals.set(n, "i32");
    for (const n of ["df", "sa", "sb", "sr", "ftop", "pc24"]) this.ir.locals.set(n, "i32");
  }

  lift(): IrFunc {
    const entry = this.newBlock("entry");
    this.cur = entry;
    this.emit({ k: "syncin" });
    for (const [, n] of FLAG_NAMES) this.set(n, c32(0));
    for (const n of ["df", "sa", "sb", "sr"]) this.set(n, c32(0));
    entry.term = { k: "br", to: label(this.f.entry) };

    const blocks = [...this.f.blocks.values()].sort((a, b) => a.start - b.start);
    for (const b of blocks) {
      this.cur = this.newBlock(label(b.start));
      for (let k = 0; k < b.insns.length; k++) {
        const i = b.insns[k];
        const last = k === b.insns.length - 1;
        this.insn = i;
        if (last && this.isTerminatorInsn(b, i)) break;
        this.emit({ k: "comment", text: `${hex(i.ip)}: ${i.text}` });
        try {
          this.lower(i);
        } catch (e) {
          if (e instanceof LiftError) this.emit({ k: "trap", msg: `unsupported ${i.text} at 0x${hex(i.ip)}: ${e.message}` });
          else throw e;
        }
      }
      this.terminate(b);
    }
    return this.ir;
  }

  // ------------------------------------------------------------------------------------------
  // IR plumbing

  private newBlock(id: string): IrBlock {
    if (this.ir.blocks.has(id)) throw new Error(`duplicate block ${id}`);
    const b: IrBlock = { id, stmts: [], term: { k: "trap", msg: "unterminated block" } };
    this.ir.blocks.set(id, b);
    this.ir.order.push(id);
    return b;
  }

  private synthBlock(): IrBlock {
    return this.newBlock(`${this.cur.id}_${++this.synthN}`);
  }

  private emit(s: Stmt): void {
    this.cur.stmts.push(s);
  }

  private set(name: string, v: Expr, ty: Ty = v.ty): void {
    if (!this.ir.locals.has(name)) this.ir.locals.set(name, ty);
    this.emit({ k: "set", name, ty, v });
  }

  /** Evaluate `v` once into a fresh temporary (registers are copied: they may be overwritten). */
  private tmp(v: Expr): Expr {
    if (v.k === "const") return v;
    const name = `t${++this.tmpN}`;
    this.set(name, v);
    return loc(name, v.ty);
  }

  private trapHere(msg: string): never {
    throw new LiftError(msg);
  }

  // ------------------------------------------------------------------------------------------
  // Registers, memory and operands

  private gpr(r: Register): { name: string; part: "d" | "w" | "l" | "h" } {
    const idx = gprIndex(r);
    if (idx < 0) this.trapHere(`register ${Register[r]}`);
    const name = GPR_NAMES[idx];
    const bits = regBits(r);
    if (bits === 32) return { name, part: "d" };
    if (bits === 16) return { name, part: "w" };
    const isHigh = r === Register.AH || r === Register.CH || r === Register.DH || r === Register.BH;
    return { name, part: isHigh ? "h" : "l" };
  }

  private getReg(r: Register): Expr {
    const { name, part } = this.gpr(r);
    const v = loc(name);
    if (part === "d") return v;
    if (part === "w") return and(v, c32(0xffff));
    if (part === "l") return and(v, c32(0xff));
    return and(shru(v, c32(8)), c32(0xff));
  }

  private setReg(r: Register, value: Expr): void {
    const { name, part } = this.gpr(r);
    const v = loc(name);
    if (part === "d") this.set(name, value);
    else if (part === "w") this.set(name, or(and(v, c32(0xffff0000)), and(value, c32(0xffff))));
    else if (part === "l") this.set(name, or(and(v, c32(0xffffff00)), and(value, c32(0xff))));
    else this.set(name, or(and(v, c32(0xffff00ff)), shl(and(value, c32(0xff)), c32(8))));
  }

  private ea(m: MemOperand): Expr {
    let a: Expr | null = null;
    if (m.base !== Register.None) a = this.getReg(m.base);
    if (m.index !== Register.None) {
      const idx = this.getReg(m.index);
      const scaled = m.scale === 1 ? idx : shl(idx, c32(Math.log2(m.scale)));
      a = a ? add(a, scaled) : scaled;
    }
    if (m.disp !== 0 || !a) a = a ? add(a, c32(m.disp)) : c32(m.disp);
    if (m.seg === Register.FS) a = add(a, cpu("fs_base"));
    else if (m.seg === Register.GS) this.trapHere("GS segment");
    return a;
  }

  private loadMem(m: MemOperand, bytes: 1 | 2 | 4 | 8, addr?: Expr): Expr {
    return load(bytes, addr ?? this.ea(m));
  }

  private store(bytes: 1 | 2 | 4 | 8, addr: Expr, v: Expr): void {
    this.emit({ k: "store", bytes, addr, v });
  }

  private opBits(o: Operand): number {
    if (o.kind === "reg") return regBits(o.reg);
    if (o.kind === "mem") return o.mem.size * 8;
    return 32;
  }

  /** Read an integer operand, zero-extended to i32. */
  private read(o: Operand, addr?: Expr): Expr {
    switch (o.kind) {
      case "reg":
        return this.getReg(o.reg);
      case "imm":
        return c32(o.value);
      case "mem": {
        const bytes = o.mem.size as 1 | 2 | 4;
        if (bytes !== 1 && bytes !== 2 && bytes !== 4) this.trapHere(`integer memory operand of ${o.mem.size} bytes`);
        return this.loadMem(o.mem, bytes, addr);
      }
      default:
        return this.trapHere(`operand kind ${o.kind}`);
    }
  }

  private write(o: Operand, v: Expr, addr?: Expr): void {
    if (o.kind === "reg") this.setReg(o.reg, v);
    else if (o.kind === "mem") this.store(o.mem.size as 1 | 2 | 4, addr ?? this.ea(o.mem), v);
    else this.trapHere(`write to ${o.kind}`);
  }

  private push(v: Expr): void {
    const val = this.tmp(v);
    this.set("esp", sub(loc("esp"), c32(4)));
    this.store(4, loc("esp"), val);
  }

  private pop(): Expr {
    const v = this.tmp(load(4, loc("esp")));
    this.set("esp", add(loc("esp"), c32(4)));
    return v;
  }

  // ------------------------------------------------------------------------------------------
  // Flags

  private flagExpr(kind: DefKind, bits: number, flag: number, a: Expr, b: Expr, r: Expr): Expr {
    const M = c32(MASK(bits));
    const S = c32(SIGN(bits));
    switch (flag) {
      case ZF:
        return eq(and(r, M), c32(0));
      case SF:
        return ne(and(r, S), c32(0));
      case PF:
        return call("parity8", "i32", r);
    }
    switch (kind) {
      case "add":
        if (flag === CF) return bin("ltu", and(r, M), and(a, M));
        if (flag === OF) return ne(and(and(xor(a, r), xor(b, r)), S), c32(0));
        return ne(and(xor(xor(a, b), r), c32(0x10)), c32(0));
      case "sub":
        if (flag === CF) return bin("ltu", and(a, M), and(b, M));
        if (flag === OF) return ne(and(and(xor(a, b), xor(a, r)), S), c32(0));
        return ne(and(xor(xor(a, b), r), c32(0x10)), c32(0));
      case "logic":
        return c32(0);
      case "inc":
        if (flag === OF) return eq(and(r, M), S);
        if (flag === AF) return eq(and(r, c32(0xf)), c32(0));
        break;
      case "dec":
        if (flag === OF) return eq(and(r, M), c32(SIGN(bits) - 1));
        if (flag === AF) return eq(and(r, c32(0xf)), c32(0xf));
        break;
      case "neg":
        if (flag === CF) return ne(and(a, M), c32(0));
        if (flag === OF) return eq(and(a, M), S);
        return ne(and(a, c32(0xf)), c32(0));
    }
    throw new Error(`no ${kind} formula for flag ${flag}`);
  }

  /**
   * Record flag definitions for the current instruction. `custom(flag)` supplies formulas for
   * instructions outside the snapshot kinds (or overrides, e.g. CF for adc/sbb).
   */
  private defFlags(a: Expr, b: Expr, r: Expr, custom?: (flag: number) => Expr | null): void {
    const i = this.insn;
    const { kind, size } = defKind(i);
    if (this.plan.snapshot.has(i.ip)) {
      if (kind === "other") throw new Error(`snapshot requested for non-snapshot def at ${hex(i.ip)}`);
      this.set("sa", a);
      this.set("sb", b);
      this.set("sr", r);
    }
    const want = this.plan.materialize.get(i.ip) ?? 0;
    if (!want) return;
    const vals: [string, Expr][] = [];
    for (const [fl, name] of FLAG_NAMES) {
      if (!(want & fl)) continue;
      let v = custom?.(fl) ?? (kind === "other" ? null : this.flagExpr(kind, size, fl, a, b, r));
      // Architecturally undefined flags (e.g. SF/ZF after imul) may hold any value.
      if (v === null && fromIced(i.rflagsUndefined) & fl) v = c32(0);
      if (v === null) throw new Error(`no flag formula for ${name} at ${hex(i.ip)}: ${i.text}`);
      vals.push([name, v]);
    }
    // Evaluate all flag values before assigning any (custom formulas may read old flags).
    const tmps = vals.map(([n, v]) => [n, this.tmp(v)] as const);
    for (const [n, v] of tmps) this.set(n, v);
  }

  /** Value (0/1) of one flag as read by the current instruction. */
  private getFlag(flag: number): Expr {
    const mode = this.plan.useMode.get(this.insn.ip);
    if (mode && mode.mode === "snapshot") return this.flagExpr(mode.kind, mode.size, flag, loc("sa"), loc("sb"), loc("sr"));
    const name = FLAG_NAMES.find(([f]) => f === flag)![1];
    return loc(name);
  }

  private cond(cc: ConditionCode): Expr {
    const mode = this.plan.useMode.get(this.insn.ip);
    if (mode && mode.mode === "snapshot" && (mode.kind === "sub" || mode.kind === "logic")) {
      const fast = this.fastCond(cc, mode.kind, mode.size);
      if (fast) return fast;
    }
    const F = (f: number) => this.getFlag(f);
    const lt = () => xor(F(SF), F(OF));
    switch (cc) {
      case ConditionCode.o: return F(OF);
      case ConditionCode.no: return eqz(F(OF));
      case ConditionCode.b: return F(CF);
      case ConditionCode.ae: return eqz(F(CF));
      case ConditionCode.e: return F(ZF);
      case ConditionCode.ne: return eqz(F(ZF));
      case ConditionCode.be: return or(F(CF), F(ZF));
      case ConditionCode.a: return eqz(or(F(CF), F(ZF)));
      case ConditionCode.s: return F(SF);
      case ConditionCode.ns: return eqz(F(SF));
      case ConditionCode.p: return F(PF);
      case ConditionCode.np: return eqz(F(PF));
      case ConditionCode.l: return lt();
      case ConditionCode.ge: return eqz(lt());
      case ConditionCode.le: return or(F(ZF), lt());
      case ConditionCode.g: return eqz(or(F(ZF), lt()));
      default: return this.trapHere(`condition ${cc}`);
    }
  }

  /** Direct comparisons for cmp/sub and test/and/or/xor snapshots. */
  private fastCond(cc: ConditionCode, kind: DefKind, bits: number): Expr | null {
    const sx = (e: Expr) => (bits === 32 ? e : bits === 16 ? un("ext16s", e, "i32") : un("ext8s", e, "i32"));
    const a = loc("sa");
    const b = loc("sb");
    const r = loc("sr");
    if (kind === "sub") {
      switch (cc) {
        case ConditionCode.e: return eq(r, c32(0));
        case ConditionCode.ne: return ne(r, c32(0));
        case ConditionCode.b: return bin("ltu", a, b);
        case ConditionCode.ae: return bin("geu", a, b);
        case ConditionCode.be: return bin("leu", a, b);
        case ConditionCode.a: return bin("gtu", a, b);
        case ConditionCode.l: return bin("lts", sx(a), sx(b));
        case ConditionCode.ge: return bin("ges", sx(a), sx(b));
        case ConditionCode.le: return bin("les", sx(a), sx(b));
        case ConditionCode.g: return bin("gts", sx(a), sx(b));
        case ConditionCode.s: return bin("lts", sx(r), c32(0));
        case ConditionCode.ns: return bin("ges", sx(r), c32(0));
        default: return null;
      }
    }
    switch (cc) {
      case ConditionCode.e:
      case ConditionCode.be: return eq(r, c32(0));
      case ConditionCode.ne:
      case ConditionCode.a: return ne(r, c32(0));
      case ConditionCode.s:
      case ConditionCode.l: return bin("lts", sx(r), c32(0));
      case ConditionCode.ns:
      case ConditionCode.ge: return bin("ges", sx(r), c32(0));
      case ConditionCode.le: return bin("les", sx(r), c32(0));
      case ConditionCode.g: return bin("gts", sx(r), c32(0));
      case ConditionCode.b:
      case ConditionCode.o: return c32(0);
      case ConditionCode.ae:
      case ConditionCode.no: return c32(1);
      default: return null;
    }
  }

  // ------------------------------------------------------------------------------------------
  // Instruction lowering

  private isTerminatorInsn(b: Block, i: Insn): boolean {
    switch (b.term.kind) {
      case "jmp": case "jcc": case "switch": case "ret": case "tail-import": case "tail-indirect":
        return true;
      case "tail":
        return i.mnemonic === Mnemonic.Jmp;
      default:
        return false;
    }
  }

  private lower(i: Insn): void {
    const o = i.ops;
    switch (i.mnemonic) {
      case Mnemonic.Nop: case Mnemonic.Wait: case Mnemonic.Ffree: case Mnemonic.Fnclex: case Mnemonic.Emms: case Mnemonic.Femms:
        if (i.mnemonic === Mnemonic.Fnclex) this.emit({ k: "cpuset", field: "fsw", ty: "i32", v: and(cpu("fsw"), c32(0x7f00)) });
        return;
      case Mnemonic.Mov: {
        if (o[0].kind === "reg" && regBits(o[0].reg) === 0) this.trapHere("segment move");
        this.write(o[0], this.read(o[1]));
        return;
      }
      case Mnemonic.Movzx:
        this.write(o[0], this.read(o[1]));
        return;
      case Mnemonic.Movsx: {
        const bits = this.opBits(o[1]);
        const v = this.read(o[1]);
        this.write(o[0], bits === 8 ? un("ext8s", v, "i32") : un("ext16s", v, "i32"));
        return;
      }
      case Mnemonic.Lea: {
        if (o[1].kind !== "mem") this.trapHere("lea without memory");
        this.write(o[0], this.ea(o[1].mem));
        return;
      }
      case Mnemonic.Xchg: {
        const addr = o[0].kind === "mem" ? this.tmp(this.ea(o[0].mem)) : o[1].kind === "mem" ? this.tmp(this.ea(o[1].mem)) : undefined;
        const a = this.tmp(this.read(o[0], addr));
        const b = this.tmp(this.read(o[1], addr));
        this.write(o[0], b, addr);
        this.write(o[1], a, addr);
        return;
      }
      case Mnemonic.Push: {
        if (this.opBits(o[0]) === 16 && o[0].kind !== "imm") this.trapHere("16-bit push");
        if (o[0].kind === "imm" && i.stackIncrement === -2) this.trapHere("16-bit push");
        this.push(this.read(o[0]));
        return;
      }
      case Mnemonic.Pop: {
        const v = this.pop();
        this.write(o[0], v); // effective address uses the incremented ESP, as on hardware
        return;
      }
      case Mnemonic.Pushad: {
        const t = this.tmp(loc("esp"));
        for (const r of ["eax", "ecx", "edx", "ebx"]) this.push(loc(r));
        this.push(t);
        for (const r of ["ebp", "esi", "edi"]) this.push(loc(r));
        return;
      }
      case Mnemonic.Popad: {
        for (const r of ["edi", "esi", "ebp"]) this.set(r, this.pop());
        this.set("esp", add(loc("esp"), c32(4)));
        for (const r of ["ebx", "edx", "ecx", "eax"]) this.set(r, this.pop());
        return;
      }
      case Mnemonic.Pushfd:
        this.push(this.composeEflags());
        return;
      case Mnemonic.Popfd: {
        const v = this.pop();
        this.emit({ k: "cpuset", field: "eflags", ty: "i32", v: and(v, c32(~0x8d5)) });
        this.set("df", and(shru(v, c32(10)), c32(1)));
        const bit = (n: number) => and(shru(v, c32(n)), c32(1));
        this.defFlags(c32(0), c32(0), c32(0), (fl) => (fl === CF ? bit(0) : fl === PF ? bit(2) : fl === AF ? bit(4) : fl === ZF ? bit(6) : fl === SF ? bit(7) : bit(11)));
        return;
      }
      case Mnemonic.Lahf: {
        const F = (f: number) => this.getFlag(f);
        this.setReg(Register.AH, or(or(or(shl(F(SF), c32(7)), shl(F(ZF), c32(6))), or(shl(F(AF), c32(4)), shl(F(PF), c32(2)))), or(F(CF), c32(2))));
        return;
      }
      case Mnemonic.Sahf: {
        const ah = this.tmp(this.getReg(Register.AH));
        const bit = (n: number) => and(shru(ah, c32(n)), c32(1));
        this.defFlags(c32(0), c32(0), c32(0), (fl) => (fl === CF ? bit(0) : fl === PF ? bit(2) : fl === AF ? bit(4) : fl === ZF ? bit(6) : fl === SF ? bit(7) : null));
        return;
      }
      case Mnemonic.Cld: this.set("df", c32(0)); return;
      case Mnemonic.Std: this.set("df", c32(1)); return;
      case Mnemonic.Clc: this.defFlags(c32(0), c32(0), c32(0), () => c32(0)); return;
      case Mnemonic.Stc: this.defFlags(c32(0), c32(0), c32(0), () => c32(1)); return;
      case Mnemonic.Cmc: {
        const cf = this.tmp(this.getFlag(CF));
        this.defFlags(c32(0), c32(0), c32(0), () => eqz(cf));
        return;
      }
      case Mnemonic.Cdq:
        this.set("edx", shrs(loc("eax"), c32(31)));
        return;
      case Mnemonic.Cwde:
        this.set("eax", un("ext16s", loc("eax"), "i32"));
        return;
      case Mnemonic.Cbw:
        this.setReg(Register.AX, un("ext8s", loc("eax"), "i32"));
        return;
      case Mnemonic.Cwd:
        this.setReg(Register.DX, shrs(un("ext16s", loc("eax"), "i32"), c32(31)));
        return;
      case Mnemonic.Bswap:
        this.write(o[0], un("bswap", this.read(o[0]), "i32"));
        return;
      case Mnemonic.Leave:
        this.set("esp", loc("ebp"));
        this.set("ebp", this.pop());
        return;
      case Mnemonic.Enter: {
        const level = o[1]?.kind === "imm" ? o[1].value : 0;
        if (level !== 0) this.trapHere("enter with nesting level");
        this.push(loc("ebp"));
        this.set("ebp", loc("esp"));
        this.set("esp", sub(loc("esp"), c32(o[0].kind === "imm" ? o[0].value : 0)));
        return;
      }
      case Mnemonic.Xlatb:
        this.setReg(Register.AL, load(1, add(loc("ebx"), and(loc("eax"), c32(0xff)))));
        return;

      case Mnemonic.Add: case Mnemonic.Sub: case Mnemonic.And: case Mnemonic.Or: case Mnemonic.Xor:
      case Mnemonic.Cmp: case Mnemonic.Test: case Mnemonic.Adc: case Mnemonic.Sbb:
        return this.arith(i);
      case Mnemonic.Inc: case Mnemonic.Dec: case Mnemonic.Neg: case Mnemonic.Not:
        return this.unary(i);
      case Mnemonic.Imul: case Mnemonic.Mul:
        return this.mul(i);
      case Mnemonic.Div: case Mnemonic.Idiv:
        return this.div(i);
      case Mnemonic.Shl: case Mnemonic.Sal: case Mnemonic.Shr: case Mnemonic.Sar:
      case Mnemonic.Rol: case Mnemonic.Ror: case Mnemonic.Rcl: case Mnemonic.Rcr:
        return this.shift(i);
      case Mnemonic.Shld: case Mnemonic.Shrd:
        return this.shiftDouble(i);
      case Mnemonic.Bt: case Mnemonic.Bts: case Mnemonic.Btr: case Mnemonic.Btc:
        return this.bitTest(i);
      case Mnemonic.Bsf: case Mnemonic.Bsr: {
        const src = this.tmp(this.read(o[1]));
        const isZero = this.tmp(eqz(src));
        const v = i.mnemonic === Mnemonic.Bsf ? un("ctz", src, "i32") : sub(c32(31), un("clz", src, "i32"));
        this.write(o[0], sel(isZero, this.read(o[0]), v));
        this.defFlags(src, c32(0), c32(0), (fl) => (fl === ZF ? isZero : c32(0)));
        return;
      }
      case Mnemonic.Xadd: {
        const addr = o[0].kind === "mem" ? this.tmp(this.ea(o[0].mem)) : undefined;
        const bits = this.opBits(o[0]);
        const a = this.tmp(this.read(o[0], addr));
        const b = this.tmp(this.read(o[1]));
        const r = this.tmp(and(add(a, b), c32(MASK(bits))));
        this.write(o[1], a);
        this.write(o[0], r, addr);
        this.defFlags(a, b, r);
        return;
      }
      case Mnemonic.Cmpxchg: {
        const addr = o[0].kind === "mem" ? this.tmp(this.ea(o[0].mem)) : undefined;
        const bits = this.opBits(o[0]);
        const acc = bits === 32 ? Register.EAX : bits === 16 ? Register.AX : Register.AL;
        const a = this.tmp(this.getReg(acc));
        const d = this.tmp(this.read(o[0], addr));
        const s = this.tmp(this.read(o[1]));
        const equal = this.tmp(eq(a, d));
        this.write(o[0], sel(equal, s, d), addr);
        this.setReg(acc, sel(equal, a, d));
        this.defFlags(a, d, this.tmp(and(sub(a, d), c32(MASK(bits)))));
        return;
      }
      case Mnemonic.Sete: case Mnemonic.Setne: case Mnemonic.Setl: case Mnemonic.Setle: case Mnemonic.Setg: case Mnemonic.Setge:
      case Mnemonic.Setb: case Mnemonic.Setbe: case Mnemonic.Seta: case Mnemonic.Setae: case Mnemonic.Sets: case Mnemonic.Setns:
      case Mnemonic.Seto: case Mnemonic.Setno: case Mnemonic.Setp: case Mnemonic.Setnp:
        this.write(o[0], this.cond(i.cc));
        return;
      case Mnemonic.Cmove: case Mnemonic.Cmovne: case Mnemonic.Cmovl: case Mnemonic.Cmovle: case Mnemonic.Cmovg: case Mnemonic.Cmovge:
      case Mnemonic.Cmovb: case Mnemonic.Cmovbe: case Mnemonic.Cmova: case Mnemonic.Cmovae: case Mnemonic.Cmovs: case Mnemonic.Cmovns:
      case Mnemonic.Cmovo: case Mnemonic.Cmovno: case Mnemonic.Cmovp: case Mnemonic.Cmovnp: {
        const src = this.tmp(this.read(o[1]));
        this.write(o[0], sel(this.cond(i.cc), src, this.read(o[0])));
        return;
      }

      case Mnemonic.Movsb: case Mnemonic.Movsw: case Mnemonic.Stosb: case Mnemonic.Stosw: case Mnemonic.Stosd:
      case Mnemonic.Lodsb: case Mnemonic.Lodsw: case Mnemonic.Lodsd: case Mnemonic.Cmpsb: case Mnemonic.Cmpsw:
      case Mnemonic.Scasb: case Mnemonic.Scasw: case Mnemonic.Scasd:
        return this.stringOp(i);
      case Mnemonic.Movsd:
      case Mnemonic.Cmpsd:
        if (o[0]?.kind === "string") return this.stringOp(i);
        return this.trapHere("SSE2 scalar double");

      case Mnemonic.Call:
        return this.lowerCall(i);
      case Mnemonic.Loop: case Mnemonic.Loope: case Mnemonic.Loopne:
        this.set("ecx", sub(loc("ecx"), c32(1)));
        return;
      case Mnemonic.Jecxz:
        return;
      case Mnemonic.Cpuid:
        this.emit({ k: "syncout" });
        this.emit({ k: "helper", fn: "rt_cpuid", args: [] });
        this.emit({ k: "syncin" });
        return;
      case Mnemonic.Int3:
        this.emit({ k: "trap", msg: `int3 at 0x${hex(i.ip)}` });
        return;
    }
    if (isX87(i.mnemonic)) return this.x87(i);
    this.trapHere(`no semantics for ${Mnemonic[i.mnemonic]}`);
  }

  private composeEflags(): Expr {
    const F = (f: number) => this.getFlag(f);
    let v: Expr = or(and(cpu("eflags"), c32(~0x8d5)), c32(2));
    v = or(v, F(CF));
    v = or(v, shl(F(PF), c32(2)));
    v = or(v, shl(F(AF), c32(4)));
    v = or(v, shl(F(ZF), c32(6)));
    v = or(v, shl(F(SF), c32(7)));
    v = or(v, shl(loc("df"), c32(10)));
    v = or(v, shl(F(OF), c32(11)));
    return v;
  }

  private arith(i: Insn): void {
    const o = i.ops;
    const bits = this.opBits(o[0]);
    const M = c32(MASK(bits));
    const addr = o[0].kind === "mem" ? this.tmp(this.ea(o[0].mem)) : undefined;
    const a = this.tmp(this.read(o[0], addr));
    const b = this.tmp(and(this.read(o[1]), M));
    let r: Expr;
    let custom: ((f: number) => Expr | null) | undefined;
    switch (i.mnemonic) {
      case Mnemonic.Add:
        r = this.tmp(and(add(a, b), M));
        break;
      case Mnemonic.Sub:
      case Mnemonic.Cmp:
        r = this.tmp(and(sub(a, b), M));
        break;
      case Mnemonic.And:
      case Mnemonic.Test:
        r = this.tmp(and(a, b));
        break;
      case Mnemonic.Or:
        r = this.tmp(or(a, b));
        break;
      case Mnemonic.Xor:
        r = this.tmp(xor(a, b));
        break;
      case Mnemonic.Adc: {
        const cin = this.tmp(this.getFlag(CF));
        r = this.tmp(and(add(add(a, b), cin), M));
        const rr = r;
        custom = (fl) => {
          if (fl === CF) return sel(cin, bin("leu", rr, a), bin("ltu", rr, a));
          if (fl === OF) return ne(and(and(xor(a, rr), xor(b, rr)), c32(SIGN(bits))), c32(0));
          if (fl === AF) return ne(and(xor(xor(a, b), rr), c32(0x10)), c32(0));
          return null;
        };
        break;
      }
      case Mnemonic.Sbb: {
        const cin = this.tmp(this.getFlag(CF));
        r = this.tmp(and(sub(sub(a, b), cin), M));
        const rr = r;
        custom = (fl) => {
          if (fl === CF) return sel(cin, bin("leu", a, b), bin("ltu", a, b));
          if (fl === OF) return ne(and(and(xor(a, b), xor(a, rr)), c32(SIGN(bits))), c32(0));
          if (fl === AF) return ne(and(xor(xor(a, b), rr), c32(0x10)), c32(0));
          return null;
        };
        break;
      }
      default:
        throw new Error("unreachable");
    }
    if (i.mnemonic !== Mnemonic.Cmp && i.mnemonic !== Mnemonic.Test) this.write(o[0], r, addr);
    if (custom) {
      const c = custom;
      const generic = (fl: number) => c(fl) ?? (fl === ZF ? eq(r, c32(0)) : fl === SF ? ne(and(r, c32(SIGN(bits))), c32(0)) : call("parity8", "i32", r));
      this.defFlags(a, b, r, generic);
    } else this.defFlags(a, b, r);
  }

  private unary(i: Insn): void {
    const o = i.ops[0];
    const bits = this.opBits(o);
    const M = c32(MASK(bits));
    const addr = o.kind === "mem" ? this.tmp(this.ea(o.mem)) : undefined;
    const a = this.tmp(this.read(o, addr));
    switch (i.mnemonic) {
      case Mnemonic.Not:
        this.write(o, and(xor(a, c32(-1)), M), addr);
        return;
      case Mnemonic.Inc: {
        const r = this.tmp(and(add(a, c32(1)), M));
        this.write(o, r, addr);
        this.defFlags(a, c32(1), r);
        return;
      }
      case Mnemonic.Dec: {
        const r = this.tmp(and(sub(a, c32(1)), M));
        this.write(o, r, addr);
        this.defFlags(a, c32(1), r);
        return;
      }
      case Mnemonic.Neg: {
        const r = this.tmp(and(sub(c32(0), a), M));
        this.write(o, r, addr);
        this.defFlags(a, c32(0), r);
        return;
      }
    }
  }

  private mul(i: Insn): void {
    const o = i.ops;
    if (o.length === 1) {
      const bits = this.opBits(o[0]);
      const signed = i.mnemonic === Mnemonic.Imul;
      const src = this.tmp(this.read(o[0]));
      if (bits === 32) {
        const ext = (e: Expr) => un(signed ? "exts" : "extu", e, "i64");
        const t = this.tmp(bin("mul", ext(loc("eax")), ext(src)));
        const lo = this.tmp(un("wrap", t, "i32"));
        const hi = this.tmp(un("wrap", bin("shru", t, c64(32n)), "i32"));
        this.set("eax", lo);
        this.set("edx", hi);
        const ovf = this.tmp(signed ? ne(hi, shrs(lo, c32(31))) : ne(hi, c32(0)));
        this.defFlags(lo, c32(0), lo, (fl) => (fl === CF || fl === OF ? ovf : null));
      } else if (bits === 16) {
        const sx = (e: Expr) => (signed ? un("ext16s", e, "i32") : and(e, c32(0xffff)));
        const t = this.tmp(bin("mul", sx(loc("eax")), sx(src)));
        this.setReg(Register.AX, t);
        this.setReg(Register.DX, shru(t, c32(16)));
        const ovf = this.tmp(signed ? ne(t, un("ext16s", t, "i32")) : ne(shru(t, c32(16)), c32(0)));
        this.defFlags(t, c32(0), t, (fl) => (fl === CF || fl === OF ? ovf : null));
      } else {
        const sx = (e: Expr) => (signed ? un("ext8s", e, "i32") : and(e, c32(0xff)));
        const t = this.tmp(bin("mul", sx(loc("eax")), sx(src)));
        this.setReg(Register.AX, t);
        const ovf = this.tmp(signed ? ne(t, un("ext8s", t, "i32")) : ne(shru(t, c32(8)), c32(0)));
        this.defFlags(t, c32(0), t, (fl) => (fl === CF || fl === OF ? ovf : null));
      }
      return;
    }
    // Two- and three-operand imul: dst = a * b (truncated); CF = OF = signed overflow.
    const bits = this.opBits(o[0]);
    const a = this.tmp(this.read(o.length === 3 ? o[1] : o[0]));
    const b = this.tmp(this.read(o.length === 3 ? o[2] : o[1]));
    if (bits === 32) {
      const r = this.tmp(bin("mul", a, b));
      this.write(o[0], r);
      const want = this.plan.materialize.get(i.ip) ?? 0;
      if (want & (CF | OF)) {
        const wide = this.tmp(bin("mul", un("exts", a, "i64"), un("exts", b, "i64")));
        const ovf = this.tmp(bin("ne", wide, un("exts", r, "i64")));
        this.defFlags(r, c32(0), r, (fl) => (fl === CF || fl === OF ? ovf : null));
      } else this.defFlags(r, c32(0), r, () => c32(0));
    } else {
      const t = this.tmp(bin("mul", un("ext16s", a, "i32"), un("ext16s", b, "i32")));
      this.write(o[0], t);
      const ovf = this.tmp(ne(t, un("ext16s", t, "i32")));
      this.defFlags(t, c32(0), t, (fl) => (fl === CF || fl === OF ? ovf : null));
    }
  }

  private div(i: Insn): void {
    const bits = this.opBits(i.ops[0]);
    const signed = i.mnemonic === Mnemonic.Idiv;
    const d = this.tmp(this.read(i.ops[0]));
    this.emit({ k: "check", c: ne(d, c32(0)), msg: `divide by zero at 0x${hex(i.ip)}` });
    if (bits === 32) {
      const dividend = this.tmp(bin("or", bin("shl", un("extu", loc("edx"), "i64"), c64(32n)), un("extu", loc("eax"), "i64")));
      const div64 = signed ? un("exts", d, "i64") : un("extu", d, "i64");
      const q = this.tmp(bin(signed ? "divs" : "divu", dividend, div64));
      const r = this.tmp(bin(signed ? "rems" : "remu", dividend, div64));
      const fits = signed ? bin("eq", q, un("exts", un("wrap", q, "i32"), "i64")) : bin("eq", bin("shru", q, c64(32n)), c64(0n));
      this.emit({ k: "check", c: fits, msg: `divide overflow at 0x${hex(i.ip)}` });
      this.set("eax", un("wrap", q, "i32"));
      this.set("edx", un("wrap", r, "i32"));
    } else if (bits === 16) {
      const dividend = this.tmp(or(shl(and(loc("edx"), c32(0xffff)), c32(16)), and(loc("eax"), c32(0xffff))));
      const q = this.tmp(bin(signed ? "divs" : "divu", dividend, signed ? un("ext16s", d, "i32") : d));
      const r = this.tmp(bin(signed ? "rems" : "remu", dividend, signed ? un("ext16s", d, "i32") : d));
      this.setReg(Register.AX, q);
      this.setReg(Register.DX, r);
    } else {
      const dividend = this.tmp(and(loc("eax"), c32(0xffff)));
      const sdiv = signed ? un("ext16s", dividend, "i32") : dividend;
      const q = this.tmp(bin(signed ? "divs" : "divu", sdiv, signed ? un("ext8s", d, "i32") : d));
      const r = this.tmp(bin(signed ? "rems" : "remu", sdiv, signed ? un("ext8s", d, "i32") : d));
      this.setReg(Register.AL, q);
      this.setReg(Register.AH, r);
    }
    this.defFlags(c32(0), c32(0), c32(0), () => c32(0));
  }

  private shift(i: Insn): void {
    const o = i.ops;
    const bits = this.opBits(o[0]);
    const M = c32(MASK(bits));
    const addr = o[0].kind === "mem" ? this.tmp(this.ea(o[0].mem)) : undefined;
    const a = this.tmp(this.read(o[0], addr));
    const dynamic = o[1].kind === "reg";
    const count = this.tmp(and(o[1].kind === "imm" ? c32(o[1].value) : this.read(o[1]), c32(0x1f)));
    if (!dynamic && (o[1].kind === "imm" ? o[1].value & 0x1f : 0) === 0) return; // no-op, flags untouched
    const m = i.mnemonic;
    let r: Expr;
    let cfx: Expr;
    let ofx: Expr;
    const msb = (e: Expr) => and(shru(e, c32(bits - 1)), c32(1));
    if (m === Mnemonic.Shl || m === Mnemonic.Sal) {
      r = this.tmp(and(shl(a, count), M));
      cfx = bits === 32 ? and(shru(a, sub(c32(32), count)), c32(1)) : and(shru(shl(a, count), c32(bits)), c32(1));
      ofx = xor(msb(r), bits === 32 ? and(shru(a, sub(c32(32), count)), c32(1)) : and(shru(shl(a, count), c32(bits)), c32(1)));
    } else if (m === Mnemonic.Shr) {
      r = this.tmp(shru(a, count));
      cfx = and(shru(a, sub(count, c32(1))), c32(1));
      ofx = msb(a);
    } else if (m === Mnemonic.Sar) {
      const sa = bits === 32 ? a : bits === 16 ? un("ext16s", a, "i32") : un("ext8s", a, "i32");
      r = this.tmp(and(shrs(sa, count), M));
      cfx = and(shrs(sa, sub(count, c32(1))), c32(1));
      ofx = c32(0);
    } else if (m === Mnemonic.Rol || m === Mnemonic.Ror) {
      const left = m === Mnemonic.Rol;
      if (bits === 32) r = this.tmp(bin(left ? "rotl" : "rotr", a, count));
      else {
        const c = this.tmp(bin("remu", count, c32(bits)));
        const rotated = left ? or(shl(a, c), shru(a, sub(c32(bits), c))) : or(shru(a, c), shl(a, sub(c32(bits), c)));
        r = this.tmp(and(rotated, M));
      }
      cfx = left ? and(r, c32(1)) : msb(r);
      ofx = left ? xor(msb(r), and(r, c32(1))) : xor(msb(r), and(shru(r, c32(bits - 2)), c32(1)));
    } else {
      // rcl / rcr through the carry flag: runtime helper returns (result | cf << 32).
      const cin = this.tmp(this.getFlag(CF));
      const t = this.tmp(call(m === Mnemonic.Rcl ? "rt_rcl" : "rt_rcr", "i64", a, count, cin, c32(bits)));
      r = this.tmp(un("wrap", t, "i32"));
      cfx = un("wrap", bin("shru", t, c64(32n)), "i32");
      ofx = m === Mnemonic.Rcl ? xor(msb(r), cfx) : xor(msb(r), and(shru(r, c32(bits - 2)), c32(1)));
    }
    this.write(o[0], dynamic ? sel(eqz(count), a, r) : r, addr);
    const isRotate = m === Mnemonic.Rol || m === Mnemonic.Ror || m === Mnemonic.Rcl || m === Mnemonic.Rcr;
    const keep = (fl: number, v: Expr) => (dynamic ? sel(eqz(count), this.getFlag(fl), v) : v);
    this.defFlags(a, count, r, (fl) => {
      if (fl === CF) return keep(CF, cfx);
      if (fl === OF) return keep(OF, ofx);
      if (isRotate) return null;
      if (fl === ZF) return keep(ZF, eq(r, c32(0)));
      if (fl === SF) return keep(SF, msb(r));
      if (fl === PF) return keep(PF, call("parity8", "i32", r));
      return keep(AF, c32(0));
    });
  }

  private shiftDouble(i: Insn): void {
    const o = i.ops;
    if (this.opBits(o[0]) !== 32) this.trapHere("16-bit shld/shrd");
    const addr = o[0].kind === "mem" ? this.tmp(this.ea(o[0].mem)) : undefined;
    const a = this.tmp(this.read(o[0], addr));
    const b = this.tmp(this.read(o[1]));
    const count = this.tmp(and(o[2].kind === "imm" ? c32(o[2].value) : this.read(o[2]), c32(0x1f)));
    const left = i.mnemonic === Mnemonic.Shld;
    const r = this.tmp(
      sel(eqz(count), a, left ? or(shl(a, count), shru(b, sub(c32(32), count))) : or(shru(a, count), shl(b, sub(c32(32), count)))),
    );
    this.write(o[0], r, addr);
    const cfx = left ? and(shru(a, sub(c32(32), count)), c32(1)) : and(shru(a, sub(count, c32(1))), c32(1));
    const keep = (fl: number, v: Expr) => sel(eqz(count), this.getFlag(fl), v);
    this.defFlags(a, b, r, (fl) =>
      fl === CF ? keep(CF, cfx) : fl === ZF ? keep(ZF, eqz(r)) : fl === SF ? keep(SF, shru(r, c32(31))) : fl === PF ? keep(PF, call("parity8", "i32", r)) : fl === OF ? keep(OF, xor(shru(r, c32(31)), shru(a, c32(31)))) : keep(AF, c32(0)),
    );
  }

  private bitTest(i: Insn): void {
    const o = i.ops;
    let addr: Expr | undefined;
    let bit: Expr;
    let width = 32;
    if (o[0].kind === "mem" && o[1].kind === "reg") {
      // Register bit offsets address memory relative to the operand (signed, unbounded).
      const off = this.tmp(this.read(o[1]));
      addr = this.tmp(add(this.ea(o[0].mem), shrs(off, c32(3))));
      bit = this.tmp(and(off, c32(7)));
      width = 8;
    } else {
      if (o[0].kind === "mem") addr = this.tmp(this.ea(o[0].mem));
      width = this.opBits(o[0]);
      bit = this.tmp(and(this.read(o[1]), c32(width - 1)));
    }
    const dst: Operand = width === 8 && o[0].kind === "mem" ? { kind: "mem", mem: { ...o[0].mem, size: 1 } } : o[0];
    const v = this.tmp(this.read(dst, addr));
    const cf = this.tmp(and(shru(v, bit), c32(1)));
    const mask = shl(c32(1), bit);
    if (i.mnemonic === Mnemonic.Bts) this.write(dst, or(v, mask), addr);
    else if (i.mnemonic === Mnemonic.Btr) this.write(dst, and(v, xor(mask, c32(-1))), addr);
    else if (i.mnemonic === Mnemonic.Btc) this.write(dst, xor(v, mask), addr);
    this.defFlags(v, bit, cf, (fl) => (fl === CF ? cf : c32(0)));
  }

  private stringOp(i: Insn): void {
    const m = i.mnemonic;
    const size =
      m === Mnemonic.Movsb || m === Mnemonic.Stosb || m === Mnemonic.Lodsb || m === Mnemonic.Cmpsb || m === Mnemonic.Scasb ? 1
      : m === Mnemonic.Movsw || m === Mnemonic.Stosw || m === Mnemonic.Lodsw || m === Mnemonic.Cmpsw || m === Mnemonic.Scasw ? 2 : 4;
    const step = this.tmp(sel(loc("df"), c32(-size), c32(size)));
    const isMovs = m === Mnemonic.Movsb || m === Mnemonic.Movsw || m === Mnemonic.Movsd;
    const isStos = m === Mnemonic.Stosb || m === Mnemonic.Stosw || m === Mnemonic.Stosd;
    const isLods = m === Mnemonic.Lodsb || m === Mnemonic.Lodsw || m === Mnemonic.Lodsd;
    const acc = size === 4 ? Register.EAX : size === 2 ? Register.AX : Register.AL;
    const repeated = i.rep || i.repne;
    if (repeated && (isMovs || isStos)) {
      // Bulk copy/fill in the runtime (byte-exact x86 semantics, including overlap).
      this.emit({ k: "syncout" });
      this.emit({ k: "helper", fn: isMovs ? "rt_rep_movs" : "rt_rep_stos", args: [c32(size), loc("df")] });
      this.emit({ k: "syncin" });
      return;
    }
    const once = () => {
      if (isMovs) {
        this.store(size as 1 | 2 | 4, loc("edi"), load(size as 1 | 2 | 4, loc("esi")));
        this.set("esi", add(loc("esi"), step));
        this.set("edi", add(loc("edi"), step));
      } else if (isStos) {
        this.store(size as 1 | 2 | 4, loc("edi"), this.getReg(acc));
        this.set("edi", add(loc("edi"), step));
      } else if (isLods) {
        this.setReg(acc, load(size as 1 | 2 | 4, loc("esi")));
        this.set("esi", add(loc("esi"), step));
      } else {
        // cmps: [esi] - [edi]; scas: acc - [edi]
        const isCmps = m === Mnemonic.Cmpsb || m === Mnemonic.Cmpsw || m === Mnemonic.Cmpsd;
        const a = this.tmp(isCmps ? load(size as 1 | 2 | 4, loc("esi")) : this.getReg(acc));
        const b = this.tmp(load(size as 1 | 2 | 4, loc("edi")));
        if (isCmps) this.set("esi", add(loc("esi"), step));
        this.set("edi", add(loc("edi"), step));
        const r = this.tmp(and(sub(a, b), c32(MASK(size * 8))));
        if (repeated) this.defFlags(a, b, r, (fl) => this.flagExpr("sub", size * 8, fl, a, b, r));
        else this.defFlags(a, b, r);
        return eq(a, b);
      }
      return null;
    };
    if (!repeated) {
      once();
      return;
    }
    if (isLods) this.trapHere("rep lods");
    // repe/repne cmps/scas: loop in IR.
    const head = this.synthBlock();
    const body = this.synthBlock();
    const exit = this.synthBlock();
    this.cur.term = { k: "br", to: head.id };
    this.cur = head;
    head.term = { k: "cbr", c: eqz(loc("ecx")), t: exit.id, f: body.id };
    this.cur = body;
    const equal = once()!;
    this.set("ecx", sub(loc("ecx"), c32(1)));
    const again = i.repne ? eqz(equal) : equal;
    body.term = { k: "cbr", c: and(again, ne(loc("ecx"), c32(0))), t: head.id, f: exit.id };
    this.cur = exit;
  }

  private lowerCall(i: Insn): void {
    const o = i.ops[0];
    if (o.kind === "branch") {
      if (i.target === i.next) {
        this.push(c32(i.next)); // call $+5: get EIP
        return;
      }
      this.push(c32(i.next));
      this.emit({ k: "syncout" });
      if (this.prog.funcs.has(i.target)) this.emit({ k: "callguest", target: i.target });
      else this.emit({ k: "callind", target: c32(i.target) });
      this.emit({ k: "syncin" });
      return;
    }
    const slot = this.prog.iatSlotOf(i);
    if (slot !== null) {
      this.push(c32(i.next));
      this.emit({ k: "syncout" });
      const imp = this.prog.pe.importBySlot.get(slot)!;
      this.emit({ k: "callhle", slot: this.importIndex(slot), name: `${imp.dll}!${imp.name}` });
      this.emit({ k: "syncin" });
      return;
    }
    // Indirect: evaluate the target before pushing (call [esp+X] reads the pre-push ESP).
    const target = this.tmp(this.read(o));
    this.push(c32(i.next));
    this.emit({ k: "syncout" });
    this.emit({ k: "callind", target });
    this.emit({ k: "syncin" });
  }

  // ------------------------------------------------------------------------------------------
  // x87

  private st(n: number): Expr {
    return cpu("fpr", "f64", and(add(loc("ftop"), c32(n)), c32(7)));
  }

  private setSt(n: number, v: Expr): void {
    this.emit({ k: "cpuset", field: "fpr", index: and(add(loc("ftop"), c32(n)), c32(7)), ty: "f64", v });
  }

  private fpush(v: Expr): void {
    const t = this.tmp(v);
    this.set("ftop", and(sub(loc("ftop"), c32(1)), c32(7)));
    this.setSt(0, t);
  }

  private fpop(): void {
    this.set("ftop", and(add(loc("ftop"), c32(1)), c32(7)));
  }

  private stIndex(o: Operand): number {
    if (o.kind !== "reg" || o.reg < Register.ST0 || o.reg > Register.ST7) this.trapHere("expected st(i)");
    return o.reg - Register.ST0;
  }

  /**
   * Round the significand to 24 bits when the x87 precision control is single (as D3D9 sets it).
   * The exponent keeps its extended range, so this is not a float cast.
   */
  private prec(v: Expr): Expr {
    const t = this.tmp(v);
    return sel(loc("pc24"), call("rt_round24", "f64", t), t);
  }

  private fmem(m: MemOperand): Expr {
    switch (m.type) {
      case MemorySize.Float32: return un("f32to64", load(4, this.ea(m), false, "f32"), "f64");
      case MemorySize.Float64: return load(8, this.ea(m), false, "f64");
      case MemorySize.Float80: return call("rt_f80_load", "f64", this.ea(m));
      case MemorySize.Int16: return un("i32tof64s", load(2, this.ea(m), true), "f64");
      case MemorySize.Int32: return un("i32tof64s", load(4, this.ea(m)), "f64");
      case MemorySize.Int64: return un("i64tof64s", load(8, this.ea(m)), "f64");
      default: return this.trapHere(`x87 memory operand ${MemorySize[m.type]}`);
    }
  }

  private fstore(m: MemOperand, v: Expr): void {
    const addr = this.ea(m);
    switch (m.type) {
      case MemorySize.Float32: this.store(4, addr, un("f64to32", v, "f32")); return;
      case MemorySize.Float64: this.store(8, addr, v); return;
      case MemorySize.Float80: this.emit({ k: "helper", fn: "rt_f80_store", args: [addr, v] }); return;
      case MemorySize.Int16: this.store(2, addr, call("rt_fist16", "i32", v, cpu("fcw"))); return;
      case MemorySize.Int32: this.store(4, addr, call("rt_fist32", "i32", v, cpu("fcw"))); return;
      case MemorySize.Int64: this.store(8, addr, call("rt_fist64", "i64", v, cpu("fcw"))); return;
      default: this.trapHere(`x87 store ${MemorySize[m.type]}`);
    }
  }

  private setC2(v: Expr): void {
    this.emit({ k: "cpuset", field: "fsw", ty: "i32", v: or(and(cpu("fsw"), c32(~0x400)), shl(v, c32(10))) });
  }

  private fcompare(a: Expr, b: Expr): void {
    const x = this.tmp(a);
    const y = this.tmp(b);
    const unord = this.tmp(or(bin("fne", x, x), bin("fne", y, y)));
    const c0 = or(bin("flt", x, y), unord);
    const c3 = or(bin("feq", x, y), unord);
    const bits = or(or(shl(c0, c32(8)), shl(unord, c32(10))), shl(c3, c32(14)));
    this.emit({ k: "cpuset", field: "fsw", ty: "i32", v: or(and(cpu("fsw"), c32(~0x4700)), bits) });
  }

  private x87(i: Insn): void {
    const o = i.ops;
    const m = i.mnemonic;
    const arith = (op: "fadd" | "fsub" | "fmul" | "fdiv", reverse: boolean, pop: boolean) => {
      let dst = 0;
      let src: Expr;
      if (o.length === 1 && o[0].kind === "mem") src = this.fmem(o[0].mem);
      else if (o.length === 2) {
        dst = this.stIndex(o[0]);
        src = this.st(this.stIndex(o[1]));
      } else this.trapHere("x87 arithmetic form");
      const d = this.tmp(this.st(dst));
      const s = this.tmp(src!);
      this.setSt(dst, this.prec(reverse ? bin(op, s, d) : bin(op, d, s)));
      if (pop) this.fpop();
    };
    switch (m) {
      case Mnemonic.Fld:
        if (o[0].kind === "mem") this.fpush(this.fmem(o[0].mem));
        else this.fpush(this.st(this.stIndex(o[0])));
        return;
      case Mnemonic.Fild:
        if (o[0].kind !== "mem") this.trapHere("fild form");
        this.fpush(this.fmem(o[0].mem));
        return;
      case Mnemonic.Fst: case Mnemonic.Fstp: case Mnemonic.Fist: case Mnemonic.Fistp: {
        const v = this.tmp(this.st(0));
        if (o[0].kind === "mem") this.fstore(o[0].mem, v);
        else this.setSt(this.stIndex(o[0]), v);
        if (m === Mnemonic.Fstp || m === Mnemonic.Fistp) this.fpop();
        return;
      }
      case Mnemonic.Fadd: return arith("fadd", false, false);
      case Mnemonic.Faddp: return arith("fadd", false, true);
      case Mnemonic.Fmul: return arith("fmul", false, false);
      case Mnemonic.Fmulp: return arith("fmul", false, true);
      case Mnemonic.Fsub: return arith("fsub", false, false);
      case Mnemonic.Fsubp: return arith("fsub", false, true);
      case Mnemonic.Fsubr: return arith("fsub", true, false);
      case Mnemonic.Fsubrp: return arith("fsub", true, true);
      case Mnemonic.Fdiv: return arith("fdiv", false, false);
      case Mnemonic.Fdivp: return arith("fdiv", false, true);
      case Mnemonic.Fdivr: return arith("fdiv", true, false);
      case Mnemonic.Fdivrp: return arith("fdiv", true, true);
      case Mnemonic.Fiadd: case Mnemonic.Fisub: case Mnemonic.Fisubr: case Mnemonic.Fimul: case Mnemonic.Fidiv: case Mnemonic.Fidivr: {
        if (o[0].kind !== "mem") this.trapHere("fi* form");
        const s = this.tmp(this.fmem(o[0].mem));
        const d = this.tmp(this.st(0));
        const op = m === Mnemonic.Fiadd ? "fadd" : m === Mnemonic.Fisub || m === Mnemonic.Fisubr ? "fsub" : m === Mnemonic.Fimul ? "fmul" : "fdiv";
        const rev = m === Mnemonic.Fisubr || m === Mnemonic.Fidivr;
        this.setSt(0, this.prec(rev ? bin(op, s, d) : bin(op, d, s)));
        return;
      }
      case Mnemonic.Fcom: case Mnemonic.Fcomp: case Mnemonic.Fucom: case Mnemonic.Fucomp: case Mnemonic.Ficom: case Mnemonic.Ficomp: {
        const b = o.length === 0 ? this.st(1) : o[o.length - 1].kind === "mem" ? this.fmem((o[o.length - 1] as { kind: "mem"; mem: MemOperand }).mem) : this.st(this.stIndex(o[o.length - 1]));
        this.fcompare(this.st(0), b);
        if (m === Mnemonic.Fcomp || m === Mnemonic.Fucomp || m === Mnemonic.Ficomp) this.fpop();
        return;
      }
      case Mnemonic.Fcompp: case Mnemonic.Fucompp:
        this.fcompare(this.st(0), this.st(1));
        this.fpop();
        this.fpop();
        return;
      case Mnemonic.Ftst:
        this.fcompare(this.st(0), cf64(0));
        return;
      case Mnemonic.Fxam:
        this.emit({ k: "cpuset", field: "fsw", ty: "i32", v: or(and(cpu("fsw"), c32(~0x4700)), call("rt_fxam", "i32", this.st(0))) });
        return;
      case Mnemonic.Fnstsw: {
        const v = or(and(cpu("fsw"), c32(~0x3800)), shl(and(loc("ftop"), c32(7)), c32(11)));
        if (o[0].kind === "reg") this.setReg(o[0].reg, v);
        else if (o[0].kind === "mem") this.store(2, this.ea(o[0].mem), v);
        return;
      }
      case Mnemonic.Fnstcw:
        if (o[0].kind !== "mem") this.trapHere("fnstcw form");
        this.store(2, this.ea(o[0].mem), cpu("fcw"));
        return;
      case Mnemonic.Fldcw: {
        if (o[0].kind !== "mem") this.trapHere("fldcw form");
        const v = this.tmp(load(2, this.ea(o[0].mem)));
        this.emit({ k: "cpuset", field: "fcw", ty: "i32", v });
        this.set("pc24", eqz(and(v, c32(0x300))));
        return;
      }
      case Mnemonic.Fninit:
        this.emit({ k: "cpuset", field: "fcw", ty: "i32", v: c32(0x37f) });
        this.emit({ k: "cpuset", field: "fsw", ty: "i32", v: c32(0) });
        this.set("ftop", c32(0));
        this.set("pc24", c32(0));
        return;
      case Mnemonic.Fxch: {
        const k = o.length === 2 ? this.stIndex(o[1]) : 1;
        const a = this.tmp(this.st(0));
        const b = this.tmp(this.st(k));
        this.setSt(0, b);
        this.setSt(k, a);
        return;
      }
      case Mnemonic.Fchs: this.setSt(0, un("fneg", this.st(0), "f64")); return;
      case Mnemonic.Fabs: this.setSt(0, un("fabs", this.st(0), "f64")); return;
      case Mnemonic.Fsqrt: this.setSt(0, this.prec(un("fsqrt", this.st(0), "f64"))); return;
      case Mnemonic.Fldz: this.fpush(cf64(0)); return;
      case Mnemonic.Fld1: this.fpush(cf64(1)); return;
      case Mnemonic.Fldpi: this.fpush(cf64(Math.PI)); return;
      case Mnemonic.Fldl2e: this.fpush(cf64(Math.LOG2E)); return;
      case Mnemonic.Fldl2t: this.fpush(cf64(Math.log2(10))); return;
      case Mnemonic.Fldlg2: this.fpush(cf64(Math.LOG10E * Math.LN2)); return;
      case Mnemonic.Fldln2: this.fpush(cf64(Math.LN2)); return;
      // Trig operands with |x| >= 2^63 are left unchanged with C2 set and nothing pushed.
      case Mnemonic.Fsin:
      case Mnemonic.Fcos: {
        const x = this.tmp(this.st(0));
        const inRange = this.tmp(eqz(bin("fge", un("fabs", x, "f64"), cf64(2 ** 63))));
        this.setSt(0, call(m === Mnemonic.Fsin ? "rt_sin" : "rt_cos", "f64", x));
        this.setC2(eqz(inRange));
        return;
      }
      case Mnemonic.Fsincos:
      case Mnemonic.Fptan: {
        const x = this.tmp(this.st(0));
        const inRange = this.tmp(eqz(bin("fge", un("fabs", x, "f64"), cf64(2 ** 63))));
        const first = this.tmp(call(m === Mnemonic.Fsincos ? "rt_sin" : "rt_tan", "f64", x));
        const pushed = this.tmp(m === Mnemonic.Fsincos ? call("rt_cos", "f64", x) : cf64(1));
        this.setSt(0, first);
        const top = this.tmp(sel(inRange, and(sub(loc("ftop"), c32(1)), c32(7)), loc("ftop")));
        this.emit({ k: "cpuset", field: "fpr", index: top, ty: "f64", v: sel(inRange, pushed, cpu("fpr", "f64", top)) });
        this.set("ftop", top);
        this.setC2(eqz(inRange));
        return;
      }
      case Mnemonic.Fpatan: {
        const y = this.tmp(this.st(1));
        const x = this.tmp(this.st(0));
        this.setSt(1, call("rt_atan2", "f64", y, x));
        this.fpop();
        return;
      }
      case Mnemonic.Fyl2x: {
        const y = this.tmp(this.st(1));
        const x = this.tmp(this.st(0));
        this.setSt(1, bin("fmul", y, call("rt_log2", "f64", x)));
        this.fpop();
        return;
      }
      case Mnemonic.F2xm1: this.setSt(0, call("rt_f2xm1", "f64", this.st(0))); return;
      case Mnemonic.Fscale: this.setSt(0, call("rt_fscale", "f64", this.st(0), this.st(1))); return;
      case Mnemonic.Frndint: this.setSt(0, call("rt_frndint", "f64", this.st(0), cpu("fcw"))); return;
      case Mnemonic.Fprem: case Mnemonic.Fprem1:
        this.emit({ k: "helper", fn: m === Mnemonic.Fprem ? "rt_fprem" : "rt_fprem1", args: [loc("ftop")] });
        return;
      case Mnemonic.Fincstp: this.fpop(); return;
      case Mnemonic.Fdecstp: this.set("ftop", and(sub(loc("ftop"), c32(1)), c32(7))); return;
      case Mnemonic.Fnsave: case Mnemonic.Fnstenv: case Mnemonic.Frstor: case Mnemonic.Fldenv: {
        if (o[0].kind !== "mem") this.trapHere("x87 env form");
        const fn = m === Mnemonic.Fnsave ? "rt_fnsave" : m === Mnemonic.Fnstenv ? "rt_fnstenv" : m === Mnemonic.Frstor ? "rt_frstor" : "rt_fldenv";
        this.emit({ k: "syncout" });
        this.emit({ k: "helper", fn, args: [this.ea(o[0].mem)] });
        this.emit({ k: "syncin" });
        return;
      }
    }
    this.trapHere(`no x87 semantics for ${Mnemonic[m]}`);
  }

  // ------------------------------------------------------------------------------------------
  // Terminators

  private terminate(b: Block): void {
    const t = b.term;
    const last = b.insns[b.insns.length - 1];
    if (last) this.insn = last;
    const done = (term: Term) => {
      this.cur.term = term;
    };
    switch (t.kind) {
      case "fall":
        return done({ k: "br", to: label(t.next) });
      case "jmp":
        if (last) this.emit({ k: "comment", text: `${hex(last.ip)}: ${last.text}` });
        return done({ k: "br", to: label(t.target) });
      case "jcc": {
        this.emit({ k: "comment", text: `${hex(last.ip)}: ${last.text}` });
        let c: Expr;
        if (last.mnemonic === Mnemonic.Jecxz) c = eqz(loc("ecx"));
        else if (last.mnemonic === Mnemonic.Loop) {
          this.set("ecx", sub(loc("ecx"), c32(1)));
          c = ne(loc("ecx"), c32(0));
        } else if (last.mnemonic === Mnemonic.Loope || last.mnemonic === Mnemonic.Loopne) {
          this.set("ecx", sub(loc("ecx"), c32(1)));
          const z = this.getFlag(ZF);
          c = and(ne(loc("ecx"), c32(0)), last.mnemonic === Mnemonic.Loope ? z : eqz(z));
        } else c = this.cond(last.cc);
        let target = label(t.target);
        if (!this.f.blocks.has(t.target)) {
          // Conditional tail call into another function.
          const cur = this.cur;
          const tb = this.synthBlock();
          this.cur = tb;
          this.emit({ k: "syncout" });
          tb.term = { k: "tail", target: t.target };
          this.cur = cur;
          target = tb.id;
        }
        return done({ k: "cbr", c, t: target, f: label(t.next) });
      }
      case "switch": {
        this.emit({ k: "comment", text: `${hex(last.ip)}: ${last.text}` });
        const m = last.mem!;
        const idx = this.getReg(m.index);
        const cur = this.cur;
        const dflt = this.synthBlock();
        dflt.term = { k: "trap", msg: `switch index out of range at 0x${hex(last.ip)}` };
        this.cur = cur;
        return done({ k: "switch", idx, targets: t.table.targets.map(label), dflt: dflt.id });
      }
      case "ret": {
        this.emit({ k: "comment", text: `${hex(last.ip)}: ${last.text}` });
        this.set("esp", add(loc("esp"), c32(4 + t.pop)));
        this.emit({ k: "syncout" });
        return done({ k: "ret" });
      }
      case "tail":
        if (last && last.mnemonic === Mnemonic.Jmp) this.emit({ k: "comment", text: `${hex(last.ip)}: ${last.text}` });
        this.emit({ k: "syncout" });
        return done({ k: "tail", target: t.target });
      case "tail-import": {
        this.emit({ k: "comment", text: `${hex(last.ip)}: ${last.text}` });
        const imp = this.prog.pe.importBySlot.get(t.slot)!;
        this.emit({ k: "syncout" });
        return done({ k: "tailhle", slot: this.importIndex(t.slot), name: `${imp.dll}!${imp.name}` });
      }
      case "tail-indirect": {
        this.emit({ k: "comment", text: `${hex(last.ip)}: ${last.text}` });
        const target = this.tmp(this.read(last.ops[0]));
        this.emit({ k: "syncout" });
        return done({ k: "tailind", target });
      }
      case "noreturn":
        return done({ k: "trap", msg: `no-return call returned at 0x${hex(last.ip)}` });
      case "trap":
        return done({ k: "trap", msg: t.reason });
    }
  }
}

const X87 = new Set<Mnemonic>([
  Mnemonic.Fld, Mnemonic.Fild, Mnemonic.Fst, Mnemonic.Fstp, Mnemonic.Fist, Mnemonic.Fistp, Mnemonic.Fadd, Mnemonic.Faddp,
  Mnemonic.Fmul, Mnemonic.Fmulp, Mnemonic.Fsub, Mnemonic.Fsubp, Mnemonic.Fsubr, Mnemonic.Fsubrp, Mnemonic.Fdiv, Mnemonic.Fdivp,
  Mnemonic.Fdivr, Mnemonic.Fdivrp, Mnemonic.Fiadd, Mnemonic.Fisub, Mnemonic.Fisubr, Mnemonic.Fimul, Mnemonic.Fidiv,
  Mnemonic.Fidivr, Mnemonic.Fcom, Mnemonic.Fcomp, Mnemonic.Fucom, Mnemonic.Fucomp, Mnemonic.Ficom, Mnemonic.Ficomp,
  Mnemonic.Fcompp, Mnemonic.Fucompp, Mnemonic.Ftst, Mnemonic.Fxam, Mnemonic.Fnstsw, Mnemonic.Fnstcw, Mnemonic.Fldcw,
  Mnemonic.Fninit, Mnemonic.Fxch, Mnemonic.Fchs, Mnemonic.Fabs, Mnemonic.Fsqrt, Mnemonic.Fldz, Mnemonic.Fld1, Mnemonic.Fldpi,
  Mnemonic.Fldl2e, Mnemonic.Fldl2t, Mnemonic.Fldlg2, Mnemonic.Fldln2, Mnemonic.Fsin, Mnemonic.Fcos, Mnemonic.Fsincos,
  Mnemonic.Fptan, Mnemonic.Fpatan, Mnemonic.Fyl2x, Mnemonic.F2xm1, Mnemonic.Fscale, Mnemonic.Frndint, Mnemonic.Fprem,
  Mnemonic.Fprem1, Mnemonic.Fincstp, Mnemonic.Fdecstp, Mnemonic.Fnsave, Mnemonic.Fnstenv, Mnemonic.Frstor, Mnemonic.Fldenv,
]);

function isX87(m: Mnemonic): boolean {
  return X87.has(m);
}

export { ALL_FLAGS, operandBits };
export type { CpuField };
