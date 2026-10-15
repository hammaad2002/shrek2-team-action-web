/**
 * Recompiler IR: typed expression trees inside basic blocks with explicit terminators.
 *
 * One IR feeds two backends (C for native debugging, WebAssembly for the browser), so it stays
 * close to the WebAssembly value model: i32/i64/f32/f64 values, guest memory loads/stores, and
 * per-function locals. Guest registers live in locals and are synchronised with the per-thread
 * `Cpu` struct at calls, returns and HLE boundaries.
 */

export type Ty = "i32" | "i64" | "f32" | "f64";

export type UnOp =
  | "neg" | "not" | "eqz" | "clz" | "ctz" | "popcnt" | "ext8s" | "ext16s" | "bswap"
  | "wrap" | "extu" | "exts" // i64<->i32
  | "fneg" | "fabs" | "fsqrt" | "fceil" | "ffloor" | "ftrunc" | "fnearest"
  | "f32to64" | "f64to32" // promote / demote
  | "i32tof64s" | "i32tof64u" | "i64tof64s" | "i32tof32s"
  | "f64toi32sat" // truncating, saturating (x86 indefinite handled by helpers)
  | "bitsf32" | "f32bits" | "bitsf64" | "f64bits"; // reinterpret

export type BinOp =
  | "add" | "sub" | "mul" | "and" | "or" | "xor" | "shl" | "shru" | "shrs" | "rotl" | "rotr"
  | "eq" | "ne" | "lts" | "ltu" | "les" | "leu" | "gts" | "gtu" | "ges" | "geu"
  | "divs" | "divu" | "rems" | "remu"
  | "fadd" | "fsub" | "fmul" | "fdiv" | "fmin" | "fmax" | "feq" | "fne" | "flt" | "fle" | "fgt" | "fge";

export type Expr =
  | { k: "const"; ty: Ty; v: number | bigint }
  | { k: "local"; ty: Ty; name: string }
  | { k: "load"; ty: Ty; bytes: 1 | 2 | 4 | 8; signed: boolean; addr: Expr }
  /** Field of the Cpu struct; `index` selects an element of an array field. */
  | { k: "cpu"; ty: Ty; field: CpuField; index?: Expr }
  | { k: "un"; op: UnOp; ty: Ty; a: Expr }
  | { k: "bin"; op: BinOp; ty: Ty; a: Expr; b: Expr }
  | { k: "select"; ty: Ty; c: Expr; a: Expr; b: Expr }
  /** Pure runtime helper (math functions, parity, conversions). */
  | { k: "call"; ty: Ty; fn: string; args: Expr[] };

export type CpuField =
  | "eax" | "ecx" | "edx" | "ebx" | "esp" | "ebp" | "esi" | "edi"
  | "eflags" | "df" | "fs_base"
  | "fpr" // f64[8]
  | "ftop" | "fcw" | "fsw" // x87 TOP, control and status words (condition bits live in fsw)
  | "mm" // i64[8]
  | "xmm" // i32[32] (8 regs x 4 lanes)
  | "mxcsr";

export type Stmt =
  | { k: "set"; name: string; ty: Ty; v: Expr }
  | { k: "store"; bytes: 1 | 2 | 4 | 8; addr: Expr; v: Expr }
  | { k: "cpuset"; field: CpuField; index?: Expr; ty: Ty; v: Expr }
  /** Write all live guest registers from locals to the Cpu struct. */
  | { k: "syncout" }
  /** Reload guest registers from the Cpu struct into locals. */
  | { k: "syncin" }
  /** Direct call to another lifted function (caller already pushed the return address). */
  | { k: "callguest"; target: number }
  /** Indirect call through the runtime dispatcher. */
  | { k: "callind"; target: Expr }
  /** Call into the HLE layer for an import (by IAT slot). */
  | { k: "callhle"; slot: number; name: string }
  /** Side-effecting runtime helper operating on the Cpu struct (cpuid, fsave, ...). */
  | { k: "helper"; fn: string; args: Expr[] }
  | { k: "trap"; msg: string }
  /** Trap with `msg` unless `c` is non-zero (e.g. x86 #DE divide errors). */
  | { k: "check"; c: Expr; msg: string }
  | { k: "comment"; text: string };

export type Term =
  | { k: "br"; to: string }
  | { k: "cbr"; c: Expr; t: string; f: string }
  | { k: "switch"; idx: Expr; targets: string[]; dflt: string }
  /** Switch on a guest code address (unbounded jump tables); unknown addresses take `dflt`. */
  | { k: "switchaddr"; target: Expr; cases: [number, string][]; dflt: string }
  | { k: "ret" }
  | { k: "tail"; target: number }
  | { k: "tailind"; target: Expr }
  | { k: "tailhle"; slot: number; name: string }
  | { k: "trap"; msg: string };

export interface IrBlock {
  id: string;
  stmts: Stmt[];
  term: Term;
}

export interface IrFunc {
  entry: number;
  name: string;
  locals: Map<string, Ty>;
  /** Block ids in emission order; the first is the entry block. */
  order: string[];
  blocks: Map<string, IrBlock>;
}

// ---------------------------------------------------------------------------------------------
// Expression constructors

export const c32 = (v: number): Expr => ({ k: "const", ty: "i32", v: v | 0 });
export const c64 = (v: bigint): Expr => ({ k: "const", ty: "i64", v });
export const cf64 = (v: number): Expr => ({ k: "const", ty: "f64", v });
export const loc = (name: string, ty: Ty = "i32"): Expr => ({ k: "local", ty, name });
export const bin = (op: BinOp, a: Expr, b: Expr, ty: Ty = a.ty): Expr => ({ k: "bin", op, ty: resultTy(op, ty), a, b } as Expr);
export const un = (op: UnOp, a: Expr, ty: Ty): Expr => ({ k: "un", op, ty, a });
export const sel = (c: Expr, a: Expr, b: Expr): Expr => ({ k: "select", ty: a.ty, c, a, b });
export const load = (bytes: 1 | 2 | 4 | 8, addr: Expr, signed = false, ty: Ty = bytes === 8 ? "i64" : "i32"): Expr => ({ k: "load", ty, bytes, signed, addr });
export const cpu = (field: CpuField, ty: Ty = "i32", index?: Expr): Expr => ({ k: "cpu", ty, field, index });
export const call = (fn: string, ty: Ty, ...args: Expr[]): Expr => ({ k: "call", ty, fn, args });

const COMPARE_OPS = new Set<BinOp>(["eq", "ne", "lts", "ltu", "les", "leu", "gts", "gtu", "ges", "geu", "feq", "fne", "flt", "fle", "fgt", "fge"]);

function resultTy(op: BinOp, operandTy: Ty): Ty {
  return COMPARE_OPS.has(op) ? "i32" : operandTy;
}

export const add = (a: Expr, b: Expr) => bin("add", a, b);
export const sub = (a: Expr, b: Expr) => bin("sub", a, b);
export const and = (a: Expr, b: Expr) => bin("and", a, b);
export const or = (a: Expr, b: Expr) => bin("or", a, b);
export const xor = (a: Expr, b: Expr) => bin("xor", a, b);
export const shl = (a: Expr, b: Expr) => bin("shl", a, b);
export const shru = (a: Expr, b: Expr) => bin("shru", a, b);
export const shrs = (a: Expr, b: Expr) => bin("shrs", a, b);
export const eq = (a: Expr, b: Expr) => bin("eq", a, b);
export const ne = (a: Expr, b: Expr) => bin("ne", a, b);
export const eqz = (a: Expr) => un("eqz", a, "i32");

/** Constant-fold trivial integer expressions to keep the output readable. */
export function isConst(e: Expr, v?: number): boolean {
  return e.k === "const" && e.ty === "i32" && (v === undefined || (e.v as number) === (v | 0));
}
