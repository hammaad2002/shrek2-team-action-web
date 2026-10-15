/**
 * WebAssembly backend: translates lifted IR functions into one module that shares the runtime's
 * memory. Semantics mirror emit-c.ts exactly (the C backend is the reference).
 *
 * Module shape:
 *  - imports "env": shared `memory`, runtime helpers (rt_hle, rt_dispatch, x87 helpers, ...),
 *    and the global `index_base` (u16 array over .text mapping guest address → table index + 1);
 *  - one function `(param $c i32)` per guest function, plus `dispatch(c, addr)`;
 *  - a funcref table holding the guest functions in address order;
 *  - custom sections "shrek2.funcs" (u32 count + u32 entry addresses in table order),
 *    "shrek2.traps" (JSON array of trap messages) and "name" (function names for debugging).
 *
 * Control flow is structured with the dominator-tree algorithm of Ramsey, "Beyond Relooper"
 * (ICFP 2022); irreducible functions fall back to a loop + br_table block dispatcher.
 */
import type { BinOp, CpuField, Expr, IrFunc, Stmt, Term, Ty, UnOp } from "./ir.ts";
import { GPR_NAMES } from "./lift.ts";
import { Bytes, OP, VT, encodeModule, typeKey, type FuncType, type ModuleSpec, type ValType } from "./wasm.ts";

const CPU_OFF: Record<CpuField, number> = {
  eax: 0, ecx: 4, edx: 8, ebx: 12, esp: 16, ebp: 20, esi: 24, edi: 28, eflags: 32, df: 36, fs_base: 40, ftop: 44,
  fcw: 48, fsw: 52, mxcsr: 56, fpr: 64, mm: 128, xmm: 192,
};
const CPU_ELEM: Partial<Record<CpuField, number>> = { fpr: 8, mm: 8, xmm: 4 };

/** Runtime functions imported from "env" (fixed order = fixed function indices). */
export const RUNTIME_IMPORTS: [string, FuncType][] = [
  ["rt_hle", { params: ["i32", "i32"], results: [] }],
  ["rt_dispatch", { params: ["i32", "i32"], results: [] }],
  ["rt_trap_id", { params: ["i32", "i32"], results: [] }],
  ["rt_cpuid", { params: ["i32"], results: [] }],
  ["rt_rep_movs", { params: ["i32", "i32", "i32"], results: [] }],
  ["rt_rep_stos", { params: ["i32", "i32", "i32"], results: [] }],
  ["rt_rcl", { params: ["i32", "i32", "i32", "i32"], results: ["i64"] }],
  ["rt_rcr", { params: ["i32", "i32", "i32", "i32"], results: ["i64"] }],
  ["rt_f80_load", { params: ["i32"], results: ["f64"] }],
  ["rt_f80_store", { params: ["i32", "i32", "f64"], results: [] }],
  ["rt_fist16", { params: ["f64", "i32"], results: ["i32"] }],
  ["rt_fist32", { params: ["f64", "i32"], results: ["i32"] }],
  ["rt_fist64", { params: ["f64", "i32"], results: ["i64"] }],
  ["rt_frndint", { params: ["f64", "i32"], results: ["f64"] }],
  ["rt_fxam", { params: ["f64"], results: ["i32"] }],
  ["rt_sin", { params: ["f64"], results: ["f64"] }],
  ["rt_cos", { params: ["f64"], results: ["f64"] }],
  ["rt_tan", { params: ["f64"], results: ["f64"] }],
  ["rt_atan2", { params: ["f64", "f64"], results: ["f64"] }],
  ["rt_log2", { params: ["f64"], results: ["f64"] }],
  ["rt_f2xm1", { params: ["f64"], results: ["f64"] }],
  ["rt_fscale", { params: ["f64", "f64"], results: ["f64"] }],
  ["rt_fprem", { params: ["i32", "i32"], results: [] }],
  ["rt_fprem1", { params: ["i32", "i32"], results: [] }],
  ["rt_fnsave", { params: ["i32", "i32"], results: [] }],
  ["rt_fnstenv", { params: ["i32", "i32"], results: [] }],
  ["rt_frstor", { params: ["i32", "i32"], results: [] }],
  ["rt_fldenv", { params: ["i32", "i32"], results: [] }],
];
const IMPORT_INDEX = new Map(RUNTIME_IMPORTS.map(([n], i) => [n, i]));

export interface WasmModuleOptions {
  /** Guest .text range covered by the dispatch index. */
  textLo: number;
  textHi: number;
}

export interface WasmOutput {
  bytes: Uint8Array;
  functions: number[];
  irreducible: number;
  traps: string[];
}

class ModuleBuilder {
  readonly types: FuncType[] = [];
  private readonly typeIdx = new Map<string, number>();
  readonly funcIndex = new Map<number, number>(); // guest entry -> wasm function index
  readonly traps: string[] = [];
  private readonly trapIdx = new Map<string, number>();
  dispatchIndex = 0;
  irreducible = 0;

  type(t: FuncType): number {
    const k = typeKey(t);
    let i = this.typeIdx.get(k);
    if (i === undefined) {
      i = this.types.length;
      this.types.push(t);
      this.typeIdx.set(k, i);
    }
    return i;
  }

  trap(msg: string): number {
    let i = this.trapIdx.get(msg);
    if (i === undefined) {
      i = this.traps.length;
      this.traps.push(msg);
      this.trapIdx.set(msg, i);
    }
    return i;
  }
}

type Frame = { kind: "loop" | "block"; id: string } | { kind: "if" };

const VT_OF: Record<Ty, ValType> = { i32: "i32", i64: "i64", f32: "f32", f64: "f64" };

class FnEmitter {
  private readonly code = new Bytes(4096);
  private readonly localIdx = new Map<string, number>();
  private readonly localTypes: ValType[] = [];
  private readonly mb: ModuleBuilder;
  private readonly f: IrFunc;

  constructor(mb: ModuleBuilder, f: IrFunc) {
    this.mb = mb;
    this.f = f;
    // Param 0 is the Cpu pointer; IR locals follow.
    for (const [name, ty] of f.locals) this.localIdx.set(name, this.newLocal(VT_OF[ty]));
  }

  private newLocal(t: ValType): number {
    this.localTypes.push(t);
    return this.localTypes.length; // index 0 is the parameter
  }

  private local(name: string): number {
    const i = this.localIdx.get(name);
    if (i === undefined) throw new Error(`${this.f.name}: unknown local ${name}`);
    return i;
  }

  // ---- Expressions -------------------------------------------------------------------------------

  private op(b: number): void {
    this.code.byte(b);
  }

  private mem(op: number, offset = 0): void {
    this.code.byte(op).u32(0).u32(offset); // alignment hint 1 byte: always valid
  }

  private i32(v: number): void {
    this.code.byte(OP.i32_const).s32(v | 0);
  }

  private callImport(name: string): void {
    const i = IMPORT_INDEX.get(name);
    if (i === undefined) throw new Error(`${this.f.name}: no runtime import ${name}`);
    this.code.byte(OP.call).u32(i);
  }

  private cpuAddr(field: CpuField, index: Expr | undefined): number {
    this.code.byte(OP.local_get).u32(0);
    if (index) {
      this.expr(index);
      this.i32(CPU_ELEM[field] ?? 4);
      this.op(OP.i32_mul);
      this.op(OP.i32_add);
    }
    return CPU_OFF[field];
  }

  private loadOp(ty: Ty, bytes: number, signed: boolean): number {
    if (ty === "f32") return OP.f32_load;
    if (ty === "f64") return OP.f64_load;
    if (ty === "i64") {
      if (bytes !== 8) throw new Error(`${this.f.name}: i64 load of ${bytes} bytes`);
      return OP.i64_load;
    }
    if (bytes === 4) return OP.i32_load;
    if (bytes === 2) return signed ? OP.i32_load16_s : OP.i32_load16_u;
    if (bytes === 1) return signed ? OP.i32_load8_s : OP.i32_load8_u;
    throw new Error(`${this.f.name}: i32 load of ${bytes} bytes`);
  }

  expr(e: Expr): void {
    switch (e.k) {
      case "const":
        switch (e.ty) {
          case "i32": this.i32(Number(e.v)); return;
          case "i64": this.code.byte(OP.i64_const).s64(BigInt(e.v)); return;
          case "f32": this.code.byte(OP.f32_const).f32(Number(e.v)); return;
          case "f64": this.code.byte(OP.f64_const).f64(Number(e.v)); return;
        }
        return;
      case "local":
        this.code.byte(OP.local_get).u32(this.local(e.name));
        return;
      case "load":
        this.expr(e.addr);
        this.mem(this.loadOp(e.ty, e.bytes, e.signed));
        return;
      case "cpu": {
        const off = this.cpuAddr(e.field, e.index);
        this.mem(this.loadOp(e.ty, e.ty === "i64" || e.ty === "f64" ? 8 : 4, false), off);
        return;
      }
      case "select":
        this.expr(e.a);
        this.expr(e.b);
        this.expr(e.c);
        this.op(OP.select);
        return;
      case "call":
        this.callExpr(e.fn, e.ty, e.args);
        return;
      case "un":
        this.unary(e.op, e.ty, e.a);
        return;
      case "bin":
        this.binary(e.op, e.a, e.b);
        return;
    }
  }

  private callExpr(fn: string, ty: Ty, args: Expr[]): void {
    if (fn === "parity8") {
      // PF: set when the low byte has an even number of bits.
      this.expr(args[0]);
      this.i32(0xff);
      this.op(OP.i32_and);
      this.op(OP.i32_popcnt);
      this.i32(1);
      this.op(OP.i32_and);
      this.op(OP.i32_eqz);
      return;
    }
    if (fn === "rt_round24") {
      // Veltkamp split: round the significand to 24 bits; keep inf/NaN/overflow unchanged.
      const x = this.newLocal("f64"), t = this.newLocal("f64"), r = this.newLocal("f64");
      this.expr(args[0]);
      this.code.byte(OP.local_tee).u32(x);
      this.code.byte(OP.f64_const).f64(536870913.0);
      this.op(OP.f64_mul);
      this.code.byte(OP.local_tee).u32(t);
      this.code.byte(OP.local_get).u32(t).byte(OP.local_get).u32(x);
      this.op(OP.f64_sub);
      this.op(OP.f64_sub);
      this.code.byte(OP.local_set).u32(r);
      // select(r, x, r == r && t - t == 0)
      this.code.byte(OP.local_get).u32(r).byte(OP.local_get).u32(x);
      this.code.byte(OP.local_get).u32(r).byte(OP.local_get).u32(r).byte(OP.f64_eq);
      this.code.byte(OP.local_get).u32(t).byte(OP.local_get).u32(t).byte(OP.f64_sub);
      this.code.byte(OP.f64_const).f64(0);
      this.op(OP.f64_eq);
      this.op(OP.i32_and);
      this.op(OP.select);
      return;
    }
    for (const a of args) this.expr(a);
    this.callImport(fn);
    void ty;
  }

  private unary(op: UnOp, ty: Ty, a: Expr): void {
    const w = a.ty === "i64";
    switch (op) {
      case "neg":
        if (w) this.code.byte(OP.i64_const).s64(0n);
        else this.i32(0);
        this.expr(a);
        this.op(w ? OP.i64_sub : OP.i32_sub);
        return;
      case "not":
        this.expr(a);
        if (w) this.code.byte(OP.i64_const).s64(-1n);
        else this.i32(-1);
        this.op(w ? OP.i64_xor : OP.i32_xor);
        return;
      case "eqz":
        this.expr(a);
        this.op(w ? OP.i64_eqz : OP.i32_eqz);
        return;
      case "clz": this.expr(a); this.op(OP.i32_clz); return;
      case "ctz": this.expr(a); this.op(OP.i32_ctz); return;
      case "popcnt": this.expr(a); this.op(OP.i32_popcnt); return;
      case "ext8s": this.expr(a); this.op(OP.i32_extend8_s); return;
      case "ext16s": this.expr(a); this.op(OP.i32_extend16_s); return;
      case "bswap": {
        const x = this.newLocal("i32");
        this.expr(a);
        this.code.byte(OP.local_set).u32(x);
        const g = () => this.code.byte(OP.local_get).u32(x);
        g(); this.i32(24); this.op(OP.i32_shr_u);
        g(); this.i32(8); this.op(OP.i32_shr_u); this.i32(0xff00); this.op(OP.i32_and); this.op(OP.i32_or);
        g(); this.i32(8); this.op(OP.i32_shl); this.i32(0xff0000); this.op(OP.i32_and); this.op(OP.i32_or);
        g(); this.i32(24); this.op(OP.i32_shl); this.op(OP.i32_or);
        return;
      }
      case "wrap": this.expr(a); this.op(OP.i32_wrap_i64); return;
      case "extu": this.expr(a); this.op(OP.i64_extend_i32_u); return;
      case "exts": this.expr(a); this.op(OP.i64_extend_i32_s); return;
      case "fneg": this.expr(a); this.op(ty === "f32" ? OP.f32_neg : OP.f64_neg); return;
      case "fabs": this.expr(a); this.op(ty === "f32" ? OP.f32_abs : OP.f64_abs); return;
      case "fsqrt": this.expr(a); this.op(ty === "f32" ? OP.f32_sqrt : OP.f64_sqrt); return;
      case "fceil": this.expr(a); this.op(OP.f64_ceil); return;
      case "ffloor": this.expr(a); this.op(OP.f64_floor); return;
      case "ftrunc": this.expr(a); this.op(OP.f64_trunc); return;
      case "fnearest": this.expr(a); this.op(OP.f64_nearest); return;
      case "f32to64": this.expr(a); this.op(OP.f64_promote_f32); return;
      case "f64to32": this.expr(a); this.op(OP.f32_demote_f64); return;
      case "i32tof64s": this.expr(a); this.op(OP.f64_convert_i32_s); return;
      case "i32tof64u": this.expr(a); this.op(OP.f64_convert_i32_u); return;
      case "i64tof64s": this.expr(a); this.op(OP.f64_convert_i64_s); return;
      case "i32tof32s": this.expr(a); this.op(OP.f32_convert_i32_s); return;
      case "f64toi32sat": this.expr(a); this.code.byte(OP.prefix_fc).u32(2); return; // i32.trunc_sat_f64_s
      case "bitsf32": this.expr(a); this.op(OP.f32_reinterpret_i32); return;
      case "f32bits": this.expr(a); this.op(OP.i32_reinterpret_f32); return;
      case "bitsf64": this.expr(a); this.op(OP.f64_reinterpret_i64); return;
      case "f64bits": this.expr(a); this.op(OP.i64_reinterpret_f64); return;
    }
  }

  private binary(op: BinOp, a: Expr, b: Expr): void {
    const w = a.ty === "i64";
    const f = a.ty === "f64" || a.ty === "f32";
    const f32 = a.ty === "f32";
    if (op === "divs" || op === "rems" || op === "divu" || op === "remu") return this.division(op, a, b, w);
    this.expr(a);
    this.expr(b);
    const I = (o32: number, o64: number) => this.op(w ? o64 : o32);
    switch (op) {
      case "add": return f ? this.op(f32 ? OP.f32_add : OP.f64_add) : I(OP.i32_add, OP.i64_add);
      case "sub": return f ? this.op(f32 ? OP.f32_sub : OP.f64_sub) : I(OP.i32_sub, OP.i64_sub);
      case "mul": return f ? this.op(f32 ? OP.f32_mul : OP.f64_mul) : I(OP.i32_mul, OP.i64_mul);
      case "and": return I(OP.i32_and, OP.i64_and);
      case "or": return I(OP.i32_or, OP.i64_or);
      case "xor": return I(OP.i32_xor, OP.i64_xor);
      // WebAssembly masks shift and rotate counts exactly like the C helpers do.
      case "shl": return I(OP.i32_shl, OP.i64_shl);
      case "shru": return I(OP.i32_shr_u, OP.i64_shr_u);
      case "shrs": return I(OP.i32_shr_s, OP.i64_shr_s);
      case "rotl": return I(OP.i32_rotl, OP.i64_rotl);
      case "rotr": return I(OP.i32_rotr, OP.i64_rotr);
      case "eq": return f ? this.op(f32 ? OP.f32_eq : OP.f64_eq) : I(OP.i32_eq, OP.i64_eq);
      case "ne": return f ? this.op(f32 ? OP.f32_ne : OP.f64_ne) : I(OP.i32_ne, OP.i64_ne);
      case "ltu": return I(OP.i32_lt_u, OP.i64_lt_u);
      case "leu": return I(OP.i32_le_u, OP.i64_le_u);
      case "gtu": return I(OP.i32_gt_u, OP.i64_gt_u);
      case "geu": return I(OP.i32_ge_u, OP.i64_ge_u);
      case "lts": return I(OP.i32_lt_s, OP.i64_lt_s);
      case "les": return I(OP.i32_le_s, OP.i64_le_s);
      case "gts": return I(OP.i32_gt_s, OP.i64_gt_s);
      case "ges": return I(OP.i32_ge_s, OP.i64_ge_s);
      case "fadd": return this.op(f32 ? OP.f32_add : OP.f64_add);
      case "fsub": return this.op(f32 ? OP.f32_sub : OP.f64_sub);
      case "fmul": return this.op(f32 ? OP.f32_mul : OP.f64_mul);
      case "fdiv": return this.op(f32 ? OP.f32_div : OP.f64_div);
      case "fmin": return this.op(OP.f64_min);
      case "fmax": return this.op(OP.f64_max);
      case "feq": return this.op(f32 ? OP.f32_eq : OP.f64_eq);
      case "fne": return this.op(f32 ? OP.f32_ne : OP.f64_ne);
      case "flt": return this.op(f32 ? OP.f32_lt : OP.f64_lt);
      case "fle": return this.op(f32 ? OP.f32_le : OP.f64_le);
      case "fgt": return this.op(f32 ? OP.f32_gt : OP.f64_gt);
      case "fge": return this.op(f32 ? OP.f32_ge : OP.f64_ge);
    }
  }

  /** Division helpers (recomp.h): unsigned x/0 = 0, signed x/-1 = -x and x%-1 = 0. Division by
   *  zero never reaches here for signed ops: the lifter checks for #DE first. */
  private division(op: BinOp, a: Expr, b: Expr, w: boolean): void {
    const t: ValType = w ? "i64" : "i32";
    const x = this.newLocal(t), y = this.newLocal(t);
    this.expr(a);
    this.code.byte(OP.local_set).u32(x);
    this.expr(b);
    this.code.byte(OP.local_set).u32(y);
    const gx = () => this.code.byte(OP.local_get).u32(x);
    const gy = () => this.code.byte(OP.local_get).u32(y);
    const k = (v: number) => (w ? this.code.byte(OP.i64_const).s64(BigInt(v)) : this.i32(v));
    if (op === "divs" || op === "rems") {
      gy();
      k(-1);
      this.op(w ? OP.i64_eq : OP.i32_eq);
      this.code.byte(OP.if).byte(VT[t]);
      if (op === "divs") {
        k(0);
        gx();
        this.op(w ? OP.i64_sub : OP.i32_sub);
      } else k(0);
      this.op(OP.else);
      gx();
      gy();
      this.op(op === "divs" ? (w ? OP.i64_div_s : OP.i32_div_s) : w ? OP.i64_rem_s : OP.i32_rem_s);
      this.op(OP.end);
    } else {
      gy();
      this.op(w ? OP.i64_eqz : OP.i32_eqz);
      this.code.byte(OP.if).byte(VT[t]);
      k(0);
      this.op(OP.else);
      gx();
      gy();
      this.op(op === "divu" ? (w ? OP.i64_div_u : OP.i32_div_u) : w ? OP.i64_rem_u : OP.i32_rem_u);
      this.op(OP.end);
    }
  }

  // ---- Statements --------------------------------------------------------------------------------

  private stmt(s: Stmt): void {
    switch (s.k) {
      case "set":
        this.expr(s.v);
        this.code.byte(OP.local_set).u32(this.local(s.name));
        return;
      case "store": {
        this.expr(s.addr);
        this.expr(s.v);
        const t = s.v.ty;
        let o: number;
        if (t === "f32") o = OP.f32_store;
        else if (t === "f64") o = OP.f64_store;
        else if (s.bytes === 8) o = OP.i64_store;
        else if (s.bytes === 4) o = OP.i32_store;
        else if (s.bytes === 2) o = OP.i32_store16;
        else o = OP.i32_store8;
        this.mem(o);
        return;
      }
      case "cpuset": {
        const off = this.cpuAddr(s.field, s.index);
        this.expr(s.v);
        const t = s.v.ty;
        this.mem(t === "f64" ? OP.f64_store : t === "i64" ? OP.i64_store : t === "f32" ? OP.f32_store : OP.i32_store, off);
        return;
      }
      case "syncout":
        for (const g of GPR_NAMES) this.storeCpu(g, this.local(g));
        this.storeCpu("ftop", this.local("ftop"));
        this.storeCpu("df", this.local("df"));
        return;
      case "syncin":
        for (const g of GPR_NAMES) this.loadCpu(g, this.local(g));
        this.loadCpu("ftop", this.local("ftop"));
        // pc24 = (fcw & 0x300) == 0
        this.code.byte(OP.local_get).u32(0);
        this.mem(OP.i32_load, CPU_OFF.fcw);
        this.i32(0x300);
        this.op(OP.i32_and);
        this.op(OP.i32_eqz);
        this.code.byte(OP.local_set).u32(this.local("pc24"));
        return;
      case "callguest":
        this.code.byte(OP.local_get).u32(0);
        this.callGuest(s.target);
        return;
      case "callind":
        this.code.byte(OP.local_get).u32(0);
        this.expr(s.target);
        this.code.byte(OP.call).u32(this.mb.dispatchIndex);
        return;
      case "callhle":
        this.code.byte(OP.local_get).u32(0);
        this.i32(s.slot);
        this.callImport("rt_hle");
        return;
      case "helper":
        this.code.byte(OP.local_get).u32(0);
        for (const a of s.args) this.expr(a);
        this.callImport(s.fn);
        return;
      case "trap":
        this.trap(s.msg);
        return;
      case "check":
        this.expr(s.c);
        this.op(OP.i32_eqz);
        this.code.byte(OP.if).byte(0x40);
        this.trap(s.msg);
        this.op(OP.end);
        return;
      case "comment":
        return;
    }
  }

  private storeCpu(field: CpuField, local: number): void {
    this.code.byte(OP.local_get).u32(0).byte(OP.local_get).u32(local);
    this.mem(OP.i32_store, CPU_OFF[field]);
  }

  private loadCpu(field: CpuField, local: number): void {
    this.code.byte(OP.local_get).u32(0);
    this.mem(OP.i32_load, CPU_OFF[field]);
    this.code.byte(OP.local_set).u32(local);
  }

  private callGuest(target: number): void {
    const fi = this.mb.funcIndex.get(target);
    if (fi !== undefined) this.code.byte(OP.call).u32(fi);
    else {
      // Not a lifted function (should not happen): resolve at run time.
      this.i32(target);
      this.code.byte(OP.call).u32(this.mb.dispatchIndex);
    }
  }

  private trap(msg: string): void {
    this.code.byte(OP.local_get).u32(0);
    this.i32(this.mb.trap(msg));
    this.callImport("rt_trap_id");
    this.op(OP.unreachable);
  }

  // ---- Control flow ------------------------------------------------------------------------------

  private succs(t: Term): string[] {
    switch (t.k) {
      case "br": return [t.to];
      case "cbr": return [t.t, t.f];
      case "switch": return [...t.targets, t.dflt];
      case "switchaddr": return [...t.cases.map((c) => c[1]), t.dflt];
      default: return [];
    }
  }

  private rpo = new Map<string, number>();
  private order: string[] = [];
  private idom = new Map<string, string>();
  private children = new Map<string, string[]>();
  private loopHeaders = new Set<string>();
  private merges = new Set<string>();

  /** Returns false when the CFG is irreducible. */
  private analyze(): boolean {
    const blocks = this.f.blocks;
    const entry = this.f.order[0];
    // Iterative DFS for reverse postorder.
    const post: string[] = [];
    const seen = new Set<string>([entry]);
    const stack: { id: string; next: number; s: string[] }[] = [{ id: entry, next: 0, s: this.succs(blocks.get(entry)!.term) }];
    while (stack.length) {
      const top = stack[stack.length - 1];
      if (top.next < top.s.length) {
        const n = top.s[top.next++];
        if (!seen.has(n)) {
          seen.add(n);
          const b = blocks.get(n);
          if (!b) throw new Error(`${this.f.name}: branch to missing block ${n}`);
          stack.push({ id: n, next: 0, s: this.succs(b.term) });
        }
      } else {
        post.push(top.id);
        stack.pop();
      }
    }
    this.order = post.reverse();
    this.order.forEach((id, i) => this.rpo.set(id, i));
    const preds = new Map<string, string[]>(this.order.map((id) => [id, []]));
    for (const id of this.order) for (const s of this.succs(blocks.get(id)!.term)) preds.get(s)!.push(id);
    // Dominators (Cooper, Harvey, Kennedy).
    this.idom.set(entry, entry);
    const intersect = (a: string, b: string) => {
      while (a !== b) {
        while (this.rpo.get(a)! > this.rpo.get(b)!) a = this.idom.get(a)!;
        while (this.rpo.get(b)! > this.rpo.get(a)!) b = this.idom.get(b)!;
      }
      return a;
    };
    for (let changed = true; changed; ) {
      changed = false;
      for (const id of this.order) {
        if (id === entry) continue;
        let nd: string | undefined;
        for (const p of preds.get(id)!) {
          if (!this.idom.has(p)) continue;
          nd = nd === undefined ? p : intersect(p, nd);
        }
        if (nd !== undefined && this.idom.get(id) !== nd) {
          this.idom.set(id, nd);
          changed = true;
        }
      }
    }
    const dominates = (a: string, b: string) => {
      for (;;) {
        if (a === b) return true;
        const up = this.idom.get(b)!;
        if (up === b) return false;
        b = up;
      }
    };
    const fwdIn = new Map<string, number>();
    for (const id of this.order) {
      const t = blocks.get(id)!.term;
      const multi = t.k === "switch" || t.k === "switchaddr";
      for (const s of this.succs(t)) {
        if (this.rpo.get(s)! <= this.rpo.get(id)!) {
          if (!dominates(s, id)) return false; // retreating edge that is not a back edge
          this.loopHeaders.add(s);
        } else {
          fwdIn.set(s, (fwdIn.get(s) ?? 0) + 1);
          if (multi) this.merges.add(s);
        }
      }
    }
    for (const [id, n] of fwdIn) if (n >= 2) this.merges.add(id);
    for (const id of this.order) {
      if (id === entry) continue;
      const p = this.idom.get(id)!;
      let c = this.children.get(p);
      if (!c) this.children.set(p, (c = []));
      c.push(id);
    }
    return true;
  }

  private depth(ctx: Frame[], kind: "loop" | "block", id: string): number {
    for (let i = ctx.length - 1; i >= 0; i--) {
      const fr = ctx[i];
      if (fr.kind === kind && fr.id === id) return ctx.length - 1 - i;
    }
    throw new Error(`${this.f.name}: no ${kind} label for ${id}`);
  }

  private doTree(x: string, ctx: Frame[]): void {
    const merges = (this.children.get(x) ?? []).filter((c) => this.merges.has(c)).sort((a, b) => this.rpo.get(b)! - this.rpo.get(a)!);
    if (this.loopHeaders.has(x)) {
      this.code.byte(OP.loop).byte(0x40);
      ctx.push({ kind: "loop", id: x });
      this.nodeWithin(x, merges, 0, ctx);
      ctx.pop();
      this.op(OP.end);
    } else {
      this.nodeWithin(x, merges, 0, ctx);
    }
  }

  private nodeWithin(x: string, ys: string[], k: number, ctx: Frame[]): void {
    if (k < ys.length) {
      const y = ys[k];
      this.code.byte(OP.block).byte(0x40);
      ctx.push({ kind: "block", id: y });
      this.nodeWithin(x, ys, k + 1, ctx);
      ctx.pop();
      this.op(OP.end);
      this.doTree(y, ctx);
      return;
    }
    const b = this.f.blocks.get(x)!;
    for (const s of b.stmts) this.stmt(s);
    this.terminator(x, b.term, ctx);
  }

  private doBranch(from: string, to: string, ctx: Frame[]): void {
    if (this.rpo.get(to)! <= this.rpo.get(from)!) this.code.byte(OP.br).u32(this.depth(ctx, "loop", to));
    else if (this.merges.has(to)) this.code.byte(OP.br).u32(this.depth(ctx, "block", to));
    else this.doTree(to, ctx);
  }

  /** Label depth for a branch that must be a `br` (multiway branches). */
  private labelFor(from: string, to: string, ctx: Frame[]): number {
    return this.rpo.get(to)! <= this.rpo.get(from)! ? this.depth(ctx, "loop", to) : this.depth(ctx, "block", to);
  }

  private terminator(x: string, t: Term, ctx: Frame[]): void {
    switch (t.k) {
      case "br":
        this.doBranch(x, t.to, ctx);
        return;
      case "cbr":
        this.expr(t.c);
        this.code.byte(OP.if).byte(0x40);
        ctx.push({ kind: "if" });
        this.doBranch(x, t.t, ctx);
        this.op(OP.else);
        this.doBranch(x, t.f, ctx);
        ctx.pop();
        this.op(OP.end);
        return;
      case "switch": {
        this.expr(t.idx);
        this.code.byte(OP.br_table).u32(t.targets.length);
        for (const l of t.targets) this.code.u32(this.labelFor(x, l, ctx));
        this.code.u32(this.labelFor(x, t.dflt, ctx));
        return;
      }
      case "switchaddr": {
        const v = this.newLocal("i32");
        this.expr(t.target);
        this.code.byte(OP.local_set).u32(v);
        for (const [a, l] of t.cases) {
          this.code.byte(OP.local_get).u32(v);
          this.i32(a);
          this.op(OP.i32_eq);
          this.code.byte(OP.br_if).u32(this.labelFor(x, l, ctx));
        }
        this.code.byte(OP.br).u32(this.labelFor(x, t.dflt, ctx));
        return;
      }
      default:
        this.leave(t);
    }
  }

  /** Terminators that leave the function. */
  private leave(t: Term): void {
    switch (t.k) {
      case "ret":
        this.op(OP.return);
        return;
      case "tail":
        this.code.byte(OP.local_get).u32(0);
        this.callGuest(t.target);
        this.op(OP.return);
        return;
      case "tailind":
        this.code.byte(OP.local_get).u32(0);
        this.expr(t.target);
        this.code.byte(OP.call).u32(this.mb.dispatchIndex);
        this.op(OP.return);
        return;
      case "tailhle":
        this.code.byte(OP.local_get).u32(0);
        this.i32(t.slot);
        this.callImport("rt_hle");
        this.op(OP.return);
        return;
      case "trap":
        this.trap(t.msg);
        return;
      default:
        throw new Error(`${this.f.name}: unexpected terminator ${t.k}`);
    }
  }

  /** Irreducible control flow: loop + br_table dispatcher over all reachable blocks. */
  private dispatcher(): void {
    const ids = this.order;
    const index = new Map(ids.map((id, i) => [id, i]));
    const lbl = this.newLocal("i32"); // starts at 0 = entry
    const n = ids.length;
    this.code.byte(OP.loop).byte(0x40);
    for (let i = 0; i < n; i++) this.code.byte(OP.block).byte(0x40);
    this.code.byte(OP.local_get).u32(lbl);
    this.code.byte(OP.br_table).u32(n - 1);
    for (let i = 0; i < n - 1; i++) this.code.u32(i);
    this.code.u32(n - 1);
    for (let i = 0; i < n; i++) {
      this.op(OP.end); // code for block i follows the end of its block
      const b = this.f.blocks.get(ids[i])!;
      for (const s of b.stmts) this.stmt(s);
      const loopDepth = n - 1 - i; // enclosing blocks i+1..n-1, then the loop
      const go = (to: string, extra = 0) => {
        this.i32(index.get(to)!);
        this.code.byte(OP.local_set).u32(lbl);
        this.code.byte(OP.br).u32(loopDepth + extra);
      };
      const t = b.term;
      switch (t.k) {
        case "br":
          go(t.to);
          break;
        case "cbr":
          this.expr(t.c);
          this.code.byte(OP.if).byte(0x40);
          go(t.t, 1);
          this.op(OP.else);
          go(t.f, 1);
          this.op(OP.end);
          break;
        case "switch": {
          const v = this.newLocal("i32");
          this.expr(t.idx);
          this.code.byte(OP.local_set).u32(v);
          t.targets.forEach((to, j) => {
            this.code.byte(OP.local_get).u32(v);
            this.i32(j);
            this.op(OP.i32_eq);
            this.code.byte(OP.if).byte(0x40);
            go(to, 1);
            this.op(OP.end);
          });
          go(t.dflt);
          break;
        }
        case "switchaddr": {
          const v = this.newLocal("i32");
          this.expr(t.target);
          this.code.byte(OP.local_set).u32(v);
          for (const [a, to] of t.cases) {
            this.code.byte(OP.local_get).u32(v);
            this.i32(a);
            this.op(OP.i32_eq);
            this.code.byte(OP.if).byte(0x40);
            go(to, 1);
            this.op(OP.end);
          }
          go(t.dflt);
          break;
        }
        default:
          this.leave(t);
      }
    }
    this.op(OP.end); // loop
  }

  emit(): Uint8Array {
    if (this.analyze()) {
      this.doTree(this.f.order[0], []);
    } else {
      this.mb.irreducible++;
      this.dispatcher();
    }
    this.op(OP.end);
    // Locals: run-length groups in index order (after the parameter).
    const body = new Bytes(this.code.len + 64);
    const groups: [ValType, number][] = [];
    for (const t of this.localTypes) {
      const last = groups[groups.length - 1];
      if (last && last[0] === t) last[1]++;
      else groups.push([t, 1]);
    }
    body.u32(groups.length);
    for (const [t, n] of groups) body.u32(n).byte(VT[t]);
    body.bytes(this.code.view());
    return body.view().slice();
  }
}

// ---- Module ------------------------------------------------------------------------------------

/**
 * Streaming module writer: the guest function list is fixed up front (it determines function
 * indices); lifted functions are then added one at a time, in address order, so callers never
 * need to keep the IR of the whole program in memory.
 */
export class WasmModuleWriter {
  private readonly mb = new ModuleBuilder();
  private readonly entries: number[];
  private readonly opts: WasmModuleOptions;
  private readonly bodies: Uint8Array[] = [];
  private readonly names: string[] = [];
  private readonly guestType: number;
  private readonly dispatchType: number;
  private next = 0;

  constructor(entries: number[], opts: WasmModuleOptions) {
    this.entries = [...entries].sort((a, b) => a - b);
    this.opts = opts;
    const nImports = RUNTIME_IMPORTS.length;
    this.entries.forEach((e, i) => this.mb.funcIndex.set(e, nImports + i));
    this.mb.dispatchIndex = nImports + this.entries.length;
    this.guestType = this.mb.type({ params: ["i32"], results: [] });
    this.dispatchType = this.mb.type({ params: ["i32", "i32"], results: [] });
    for (const [, t] of RUNTIME_IMPORTS) this.mb.type(t);
  }

  add(f: IrFunc): void {
    if (f.entry !== this.entries[this.next]) throw new Error(`function 0x${f.entry.toString(16)} added out of order`);
    this.next++;
    this.bodies.push(new FnEmitter(this.mb, f).emit());
    this.names.push(f.name);
  }

  get irreducible(): number {
    return this.mb.irreducible;
  }

  finish(): WasmOutput {
    if (this.next !== this.entries.length) throw new Error(`only ${this.next} of ${this.entries.length} functions added`);
    const mb = this.mb;
    const nImports = RUNTIME_IMPORTS.length;
    const imports = RUNTIME_IMPORTS.map(([name, t]) => ({ module: "env", name, kind: "func" as const, type: mb.type(t) }));
    const bodies = this.bodies;
    // dispatch(c, addr): guest functions through the u16 index at `index_base`, else the runtime.
    {
      const textSize = this.opts.textHi - this.opts.textLo;
      const b = new Bytes(128);
      b.u32(1).u32(2).byte(VT.i32); // locals: $off, $i
      const C = 0, A = 1, OFF = 2, I = 3;
      b.byte(OP.local_get).u32(A).byte(OP.i32_const).s32(this.opts.textLo).byte(OP.i32_sub).byte(OP.local_tee).u32(OFF);
      b.byte(OP.i32_const).s32(textSize).byte(OP.i32_lt_u).byte(OP.if).byte(0x40);
      b.byte(OP.global_get).u32(0).byte(OP.local_get).u32(OFF).byte(OP.i32_const).s32(1).byte(OP.i32_shl).byte(OP.i32_add);
      b.byte(OP.i32_load16_u).u32(0).u32(0).byte(OP.local_tee).u32(I);
      b.byte(OP.if).byte(0x40);
      b.byte(OP.local_get).u32(C).byte(OP.local_get).u32(I).byte(OP.i32_const).s32(1).byte(OP.i32_sub);
      b.byte(OP.call_indirect).u32(this.guestType).u32(0);
      b.byte(OP.return).byte(OP.end).byte(OP.end);
      b.byte(OP.local_get).u32(C).byte(OP.local_get).u32(A).byte(OP.call).u32(IMPORT_INDEX.get("rt_dispatch")!);
      b.byte(OP.end);
      bodies.push(b.view().slice());
    }
    const n = this.entries.length;
    const fl = new Uint8Array(4 + 4 * n);
    const dv = new DataView(fl.buffer);
    dv.setUint32(0, n, true);
    this.entries.forEach((e, i) => dv.setUint32(4 + 4 * i, e, true));
    const names = new Bytes(1 << 16);
    {
      const sub = new Bytes(1 << 16);
      sub.u32(n + 1);
      this.names.forEach((name, i) => sub.u32(nImports + i).str(name));
      sub.u32(mb.dispatchIndex).str("dispatch");
      names.byte(1).sized(sub);
    }
    const spec: ModuleSpec = {
      types: mb.types,
      imports,
      memoryImport: { module: "env", name: "memory", min: 1, max: 65536, shared: true },
      globalImports: [{ module: "env", name: "index_base", type: "i32" }],
      funcs: [...this.entries.map(() => this.guestType), this.dispatchType],
      bodies,
      table: { size: n, elems: this.entries.map((_, i) => nImports + i) },
      exports: [{ name: "dispatch", kind: "func", index: mb.dispatchIndex }],
      custom: [
        { name: "shrek2.funcs", data: fl },
        { name: "shrek2.traps", data: new TextEncoder().encode(JSON.stringify(mb.traps)) },
        { name: "name", data: names.view().slice() },
      ],
    };
    return { bytes: encodeModule(spec), functions: this.entries, irreducible: mb.irreducible, traps: mb.traps };
  }
}

/** Translate lifted functions into one WebAssembly module (see the file comment for its shape). */
export function emitWasmModule(funcs: IrFunc[], opts: WasmModuleOptions): WasmOutput {
  const sorted = [...funcs].sort((a, b) => a.entry - b.entry);
  const w = new WasmModuleWriter(sorted.map((f) => f.entry), opts);
  for (const f of sorted) w.add(f);
  return w.finish();
}
