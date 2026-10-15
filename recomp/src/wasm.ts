/**
 * Minimal WebAssembly binary writer (MVP + bulk memory + threads + saturating conversions +
 * sign extension), enough for the recompiler's WebAssembly backend.
 */

export type ValType = "i32" | "i64" | "f32" | "f64";
export const VT: Record<ValType, number> = { i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c };

/** Growable byte buffer with LEB128 helpers. */
export class Bytes {
  buf: Uint8Array;
  len = 0;

  constructor(cap = 256) {
    this.buf = new Uint8Array(cap);
  }

  private grow(n: number): void {
    if (this.len + n <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + n) cap *= 2;
    const nb = new Uint8Array(cap);
    nb.set(this.buf.subarray(0, this.len));
    this.buf = nb;
  }

  byte(b: number): this {
    this.grow(1);
    this.buf[this.len++] = b & 0xff;
    return this;
  }

  bytes(b: Uint8Array): this {
    this.grow(b.length);
    this.buf.set(b, this.len);
    this.len += b.length;
    return this;
  }

  u32(v: number): this {
    v >>>= 0;
    do {
      let b = v & 0x7f;
      v >>>= 7;
      if (v) b |= 0x80;
      this.byte(b);
    } while (v);
    return this;
  }

  s32(v: number): this {
    v |= 0;
    for (;;) {
      const b = v & 0x7f;
      v >>= 7;
      if ((v === 0 && (b & 0x40) === 0) || (v === -1 && (b & 0x40) !== 0)) {
        this.byte(b);
        return this;
      }
      this.byte(b | 0x80);
    }
  }

  s64(v: bigint): this {
    v = BigInt.asIntN(64, v);
    for (;;) {
      const b = Number(v & 0x7fn);
      v >>= 7n;
      if ((v === 0n && (b & 0x40) === 0) || (v === -1n && (b & 0x40) !== 0)) {
        this.byte(b);
        return this;
      }
      this.byte(b | 0x80);
    }
  }

  f32(v: number): this {
    const t = new DataView(new ArrayBuffer(4));
    t.setFloat32(0, v, true);
    return this.bytes(new Uint8Array(t.buffer));
  }

  f64(v: number): this {
    const t = new DataView(new ArrayBuffer(8));
    t.setFloat64(0, v, true);
    return this.bytes(new Uint8Array(t.buffer));
  }

  str(s: string): this {
    const b = new TextEncoder().encode(s);
    return this.u32(b.length).bytes(b);
  }

  /** Append another buffer prefixed with its length. */
  sized(b: Bytes): this {
    return this.u32(b.len).bytes(b.buf.subarray(0, b.len));
  }

  view(): Uint8Array {
    return this.buf.subarray(0, this.len);
  }
}

// Opcodes used by the backend.
export const OP = {
  unreachable: 0x00, nop: 0x01, block: 0x02, loop: 0x03, if: 0x04, else: 0x05, end: 0x0b, br: 0x0c, br_if: 0x0d,
  br_table: 0x0e, return: 0x0f, call: 0x10, call_indirect: 0x11, drop: 0x1a, select: 0x1b,
  local_get: 0x20, local_set: 0x21, local_tee: 0x22, global_get: 0x23,
  i32_load: 0x28, i64_load: 0x29, f32_load: 0x2a, f64_load: 0x2b, i32_load8_s: 0x2c, i32_load8_u: 0x2d,
  i32_load16_s: 0x2e, i32_load16_u: 0x2f, i32_store: 0x36, i64_store: 0x37, f32_store: 0x38, f64_store: 0x39,
  i32_store8: 0x3a, i32_store16: 0x3b,
  i32_const: 0x41, i64_const: 0x42, f32_const: 0x43, f64_const: 0x44,
  i32_eqz: 0x45, i32_eq: 0x46, i32_ne: 0x47, i32_lt_s: 0x48, i32_lt_u: 0x49, i32_gt_s: 0x4a, i32_gt_u: 0x4b,
  i32_le_s: 0x4c, i32_le_u: 0x4d, i32_ge_s: 0x4e, i32_ge_u: 0x4f,
  i64_eqz: 0x50, i64_eq: 0x51, i64_ne: 0x52, i64_lt_s: 0x53, i64_lt_u: 0x54, i64_gt_s: 0x55, i64_gt_u: 0x56,
  i64_le_s: 0x57, i64_le_u: 0x58, i64_ge_s: 0x59, i64_ge_u: 0x5a,
  f32_eq: 0x5b, f32_ne: 0x5c, f32_lt: 0x5d, f32_gt: 0x5e, f32_le: 0x5f, f32_ge: 0x60,
  f64_eq: 0x61, f64_ne: 0x62, f64_lt: 0x63, f64_gt: 0x64, f64_le: 0x65, f64_ge: 0x66,
  i32_clz: 0x67, i32_ctz: 0x68, i32_popcnt: 0x69, i32_add: 0x6a, i32_sub: 0x6b, i32_mul: 0x6c, i32_div_s: 0x6d,
  i32_div_u: 0x6e, i32_rem_s: 0x6f, i32_rem_u: 0x70, i32_and: 0x71, i32_or: 0x72, i32_xor: 0x73, i32_shl: 0x74,
  i32_shr_s: 0x75, i32_shr_u: 0x76, i32_rotl: 0x77, i32_rotr: 0x78,
  i64_clz: 0x79, i64_ctz: 0x7a, i64_popcnt: 0x7b, i64_add: 0x7c, i64_sub: 0x7d, i64_mul: 0x7e, i64_div_s: 0x7f,
  i64_div_u: 0x80, i64_rem_s: 0x81, i64_rem_u: 0x82, i64_and: 0x83, i64_or: 0x84, i64_xor: 0x85, i64_shl: 0x86,
  i64_shr_s: 0x87, i64_shr_u: 0x88, i64_rotl: 0x89, i64_rotr: 0x8a,
  f32_abs: 0x8b, f32_neg: 0x8c, f32_sqrt: 0x91, f32_add: 0x92, f32_sub: 0x93, f32_mul: 0x94, f32_div: 0x95,
  f64_abs: 0x99, f64_neg: 0x9a, f64_ceil: 0x9b, f64_floor: 0x9c, f64_trunc: 0x9d, f64_nearest: 0x9e, f64_sqrt: 0x9f,
  f64_add: 0xa0, f64_sub: 0xa1, f64_mul: 0xa2, f64_div: 0xa3, f64_min: 0xa4, f64_max: 0xa5,
  i32_wrap_i64: 0xa7, i64_extend_i32_s: 0xac, i64_extend_i32_u: 0xad,
  f32_convert_i32_s: 0xb2, f32_demote_f64: 0xb6, f64_convert_i32_s: 0xb7, f64_convert_i32_u: 0xb8,
  f64_convert_i64_s: 0xb9, f64_promote_f32: 0xbb,
  i32_reinterpret_f32: 0xbc, i64_reinterpret_f64: 0xbd, f32_reinterpret_i32: 0xbe, f64_reinterpret_i64: 0xbf,
  i32_extend8_s: 0xc0, i32_extend16_s: 0xc1,
  prefix_fc: 0xfc, // i32.trunc_sat_f64_s = 0xfc 0x02
} as const;

export interface FuncType {
  params: ValType[];
  results: ValType[];
}

export const typeKey = (t: FuncType) => `${t.params.join(",")}->${t.results.join(",")}`;

export interface ModuleSpec {
  types: FuncType[];
  imports: { module: string; name: string; kind: "func"; type: number }[];
  memoryImport: { module: string; name: string; min: number; max: number; shared: boolean };
  globalImports: { module: string; name: string; type: ValType }[];
  /** Function types (indices into types) of defined functions, in index order. */
  funcs: number[];
  /** Function bodies (already encoded: locals + code + end). */
  bodies: Uint8Array[];
  table: { size: number; elems: number[] } | null;
  exports: { name: string; kind: "func" | "table"; index: number }[];
  custom: { name: string; data: Uint8Array }[];
}

export function encodeModule(m: ModuleSpec): Uint8Array {
  const out = new Bytes(1 << 20);
  out.bytes(new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]));
  const section = (id: number, body: Bytes) => {
    out.byte(id).sized(body);
  };
  // Type section.
  {
    const s = new Bytes();
    s.u32(m.types.length);
    for (const t of m.types) {
      s.byte(0x60).u32(t.params.length);
      for (const p of t.params) s.byte(VT[p]);
      s.u32(t.results.length);
      for (const r of t.results) s.byte(VT[r]);
    }
    section(1, s);
  }
  // Import section: functions, memory, globals.
  {
    const s = new Bytes();
    s.u32(m.imports.length + 1 + m.globalImports.length);
    for (const im of m.imports) s.str(im.module).str(im.name).byte(0x00).u32(im.type);
    const mem = m.memoryImport;
    s.str(mem.module).str(mem.name).byte(0x02).byte(mem.shared ? 0x03 : 0x01).u32(mem.min).u32(mem.max);
    for (const g of m.globalImports) s.str(g.module).str(g.name).byte(0x03).byte(VT[g.type]).byte(0x00);
    section(2, s);
  }
  // Function section.
  {
    const s = new Bytes();
    s.u32(m.funcs.length);
    for (const t of m.funcs) s.u32(t);
    section(3, s);
  }
  // Table section.
  if (m.table) {
    const s = new Bytes();
    s.u32(1).byte(0x70).byte(0x01).u32(m.table.size).u32(m.table.size);
    section(4, s);
  }
  // Export section.
  {
    const s = new Bytes();
    s.u32(m.exports.length);
    for (const e of m.exports) s.str(e.name).byte(e.kind === "func" ? 0x00 : 0x01).u32(e.index);
    section(7, s);
  }
  // Element section (active, table 0, offset 0).
  if (m.table) {
    const s = new Bytes();
    s.u32(1).byte(0x00).byte(OP.i32_const).s32(0).byte(OP.end).u32(m.table.elems.length);
    for (const f of m.table.elems) s.u32(f);
    section(9, s);
  }
  // Code section.
  {
    const s = new Bytes(1 << 20);
    s.u32(m.bodies.length);
    for (const b of m.bodies) s.u32(b.length).bytes(b);
    section(10, s);
  }
  for (const c of m.custom) {
    const s = new Bytes(c.data.length + 64);
    s.str(c.name).bytes(c.data);
    section(0, s);
  }
  return out.view().slice();
}
