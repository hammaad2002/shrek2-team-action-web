/**
 * C backend: one C function per lifted guest function, using runtime/include/recomp.h.
 * i32 values are uint32_t (wrapping arithmetic, no UB); signed operations cast explicitly.
 */
import type { BinOp, CpuField, Expr, IrFunc, Stmt, Term, Ty, UnOp } from "./ir.ts";
import { GPR_NAMES } from "./lift.ts";

const CTY: Record<Ty, string> = { i32: "uint32_t", i64: "uint64_t", f32: "float", f64: "double" };

function lit(e: Expr & { k: "const" }): string {
  switch (e.ty) {
    case "i32": {
      const u = (e.v as number) >>> 0;
      return u < 4096 ? `${u}u` : `0x${u.toString(16)}u`;
    }
    case "i64":
      return `0x${BigInt.asUintN(64, e.v as bigint).toString(16)}ull`;
    case "f64":
    case "f32": {
      const v = e.v as number;
      const s = Number.isInteger(v) ? `${v}.0` : `${v}`;
      return e.ty === "f32" ? `${s}f` : s;
    }
  }
}

function field(f: CpuField, index?: string): string {
  return index === undefined ? `c->${f}` : `c->${f}[${index}]`;
}

export function expr(e: Expr): string {
  switch (e.k) {
    case "const":
      return lit(e);
    case "local":
      return e.name;
    case "load": {
      const a = expr(e.addr);
      if (e.ty === "f32") return `ldf32(${a})`;
      if (e.ty === "f64") return `ldf64(${a})`;
      if (e.bytes === 8) return `ld64(${a})`;
      const base = `ld${e.bytes * 8}(${a})`;
      if (!e.signed || e.bytes === 4) return base;
      return e.bytes === 2 ? `(uint32_t)(int32_t)(int16_t)${base}` : `(uint32_t)(int32_t)(int8_t)${base}`;
    }
    case "cpu":
      return field(e.field, e.index ? expr(e.index) : undefined);
    case "select":
      return `(${expr(e.c)} ? ${expr(e.a)} : ${expr(e.b)})`;
    case "call":
      return `${e.fn === "parity8" ? "rt_parity8" : e.fn}(${e.args.map(expr).join(", ")})`;
    case "un":
      return unary(e.op, e.ty, e.a);
    case "bin":
      return binary(e.op, e.a, e.b);
  }
}

function unary(op: UnOp, ty: Ty, a: Expr): string {
  const x = expr(a);
  switch (op) {
    case "neg": return ty === "i64" ? `(0ull - ${x})` : `(0u - ${x})`;
    case "not": return `(~${x})`;
    case "eqz": return `(${x} == 0)`;
    case "clz": return `clz32(${x})`;
    case "ctz": return `ctz32(${x})`;
    case "popcnt": return `(uint32_t)__builtin_popcount(${x})`;
    case "ext8s": return `(uint32_t)(int32_t)(int8_t)(${x})`;
    case "ext16s": return `(uint32_t)(int32_t)(int16_t)(${x})`;
    case "bswap": return `__builtin_bswap32(${x})`;
    case "wrap": return `(uint32_t)(${x})`;
    case "extu": return `(uint64_t)(${x})`;
    case "exts": return `(uint64_t)(int64_t)(int32_t)(${x})`;
    case "fneg": return `(-${x})`;
    case "fabs": return `__builtin_fabs(${x})`;
    case "fsqrt": return `__builtin_sqrt(${x})`;
    case "fceil": return `__builtin_ceil(${x})`;
    case "ffloor": return `__builtin_floor(${x})`;
    case "ftrunc": return `__builtin_trunc(${x})`;
    case "fnearest": return `__builtin_nearbyint(${x})`;
    case "f32to64": return `(double)(${x})`;
    case "f64to32": return `(float)(${x})`;
    case "i32tof64s": return `(double)(int32_t)(${x})`;
    case "i32tof64u": return `(double)(${x})`;
    case "i64tof64s": return `(double)(int64_t)(${x})`;
    case "i32tof32s": return `(float)(int32_t)(${x})`;
    case "f64toi32sat": return `f2i32sat(${x})`;
    case "bitsf32": return `bits2f32(${x})`;
    case "f32bits": return `f32bits(${x})`;
    case "bitsf64": return `bits2f64(${x})`;
    case "f64bits": return `f64bits(${x})`;
  }
}

function binary(op: BinOp, a: Expr, b: Expr): string {
  const x = expr(a);
  const y = expr(b);
  const w = a.ty === "i64";
  const s = w ? "int64_t" : "int32_t";
  const mask = w ? 63 : 31;
  const cnt = b.k === "const" ? `${(Number(b.v) & mask) >>> 0}` : `(${y} & ${mask})`;
  switch (op) {
    case "add": return `(${x} + ${y})`;
    case "sub": return `(${x} - ${y})`;
    case "mul": return `(${x} * ${y})`;
    case "and": return `(${x} & ${y})`;
    case "or": return `(${x} | ${y})`;
    case "xor": return `(${x} ^ ${y})`;
    case "shl": return `(${x} << ${cnt})`;
    case "shru": return `(${x} >> ${cnt})`;
    case "shrs": return w ? `(uint64_t)((int64_t)${x} >> ${cnt})` : `(uint32_t)((int32_t)${x} >> ${cnt})`;
    case "rotl": return `rotl32(${x}, ${y})`;
    case "rotr": return `rotr32(${x}, ${y})`;
    case "eq": return `(${x} == ${y})`;
    case "ne": return `(${x} != ${y})`;
    case "ltu": return `(${x} < ${y})`;
    case "leu": return `(${x} <= ${y})`;
    case "gtu": return `(${x} > ${y})`;
    case "geu": return `(${x} >= ${y})`;
    case "lts": return `((${s})${x} < (${s})${y})`;
    case "les": return `((${s})${x} <= (${s})${y})`;
    case "gts": return `((${s})${x} > (${s})${y})`;
    case "ges": return `((${s})${x} >= (${s})${y})`;
    case "divs": return w ? `sdiv64(${x}, ${y})` : `sdiv32(${x}, ${y})`;
    case "rems": return w ? `srem64(${x}, ${y})` : `srem32(${x}, ${y})`;
    case "divu": return w ? `udiv64(${x}, ${y})` : `udiv32(${x}, ${y})`;
    case "remu": return w ? `urem64(${x}, ${y})` : `urem32(${x}, ${y})`;
    case "fadd": return `(${x} + ${y})`;
    case "fsub": return `(${x} - ${y})`;
    case "fmul": return `(${x} * ${y})`;
    case "fdiv": return `(${x} / ${y})`;
    case "fmin": return `__builtin_fmin(${x}, ${y})`;
    case "fmax": return `__builtin_fmax(${x}, ${y})`;
    case "feq": return `(${x} == ${y})`;
    case "fne": return `(${x} != ${y})`;
    case "flt": return `(${x} < ${y})`;
    case "fle": return `(${x} <= ${y})`;
    case "fgt": return `(${x} > ${y})`;
    case "fge": return `(${x} >= ${y})`;
  }
}

const cstr = (s: string) => JSON.stringify(s).replace(/\?\?/g, "?\\?");
const fname = (va: number) => `f_${va.toString(16).padStart(8, "0")}`;

function stmt(s: Stmt, out: string[]): void {
  switch (s.k) {
    case "set":
      out.push(`  ${s.name} = ${expr(s.v)};`);
      return;
    case "store": {
      const a = expr(s.addr);
      const v = expr(s.v);
      if (s.v.ty === "f32") out.push(`  stf32(${a}, ${v});`);
      else if (s.v.ty === "f64") out.push(`  stf64(${a}, ${v});`);
      else if (s.bytes === 8) out.push(`  st64(${a}, ${v});`);
      else out.push(`  st${s.bytes * 8}(${a}, ${v});`);
      return;
    }
    case "cpuset":
      out.push(`  ${field(s.field, s.index ? expr(s.index) : undefined)} = ${expr(s.v)};`);
      return;
    case "syncout":
      out.push(`  ${GPR_NAMES.map((g) => `c->${g} = ${g};`).join(" ")} c->ftop = ftop; c->df = df;`);
      return;
    case "syncin":
      out.push(`  ${GPR_NAMES.map((g) => `${g} = c->${g};`).join(" ")} ftop = c->ftop; pc24 = (c->fcw & 0x300u) == 0;`);
      return;
    case "callguest":
      out.push(`  ${fname(s.target)}(c);`);
      return;
    case "callind":
      out.push(`  rt_dispatch(c, ${expr(s.target)});`);
      return;
    case "callhle":
      out.push(`  rt_hle(c, ${s.slot}); /* ${s.name} */`);
      return;
    case "helper":
      out.push(`  ${s.fn}(c${s.args.map((a) => ", " + expr(a)).join("")});`);
      return;
    case "trap":
      out.push(`  rt_trap(c, ${cstr(s.msg)});`);
      return;
    case "check":
      out.push(`  if (!(${expr(s.c)})) rt_trap(c, ${cstr(s.msg)});`);
      return;
    case "comment":
      out.push(`  /* ${s.text.replace(/\*\//g, "* /")} */`);
      return;
  }
}

function term(t: Term, out: string[]): void {
  switch (t.k) {
    case "br":
      out.push(`  goto ${t.to};`);
      return;
    case "cbr":
      out.push(`  if (${expr(t.c)}) goto ${t.t}; else goto ${t.f};`);
      return;
    case "switch":
      out.push(`  switch (${expr(t.idx)}) {`);
      t.targets.forEach((l, i) => out.push(`    case ${i}: goto ${l};`));
      out.push(`    default: goto ${t.dflt};`, "  }");
      return;
    case "ret":
      out.push("  return;");
      return;
    case "tail":
      out.push(`  ${fname(t.target)}(c); return;`);
      return;
    case "tailind":
      out.push(`  rt_dispatch(c, ${expr(t.target)}); return;`);
      return;
    case "tailhle":
      out.push(`  rt_hle(c, ${t.slot}); return; /* ${t.name} */`);
      return;
    case "trap":
      out.push(`  rt_trap(c, ${cstr(t.msg)});`);
      return;
  }
}

export function emitFunction(f: IrFunc): string {
  const out: string[] = [];
  out.push(`void ${f.name}(Cpu* restrict c) {`);
  const byTy = new Map<Ty, string[]>();
  for (const [name, ty] of f.locals) byTy.set(ty, [...(byTy.get(ty) ?? []), name]);
  for (const [ty, names] of byTy) {
    for (let i = 0; i < names.length; i += 16) out.push(`  ${CTY[ty]} ${names.slice(i, i + 16).join(", ")};`);
  }
  // Silence unused-label/variable noise in generated code.
  for (const id of f.order) {
    const b = f.blocks.get(id)!;
    if (id !== "entry") out.push(`${id}:;`);
    for (const s of b.stmts) stmt(s, out);
    term(b.term, out);
  }
  out.push("}");
  return out.join("\n");
}

export function emitPrototypes(funcs: number[]): string {
  return funcs.map((a) => `void ${fname(a)}(Cpu* restrict c);`).join("\n");
}
