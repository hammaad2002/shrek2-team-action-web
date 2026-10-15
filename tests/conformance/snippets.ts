/**
 * Builds a synthetic PE containing one tiny test function per distinct instruction form used by
 * the game, plus a JSON manifest, for differential testing (Unicorn vs. recompiled code).
 *
 *   node tests/conformance/snippets.ts <game.exe> <ghidra-hints.json> <outdir>
 *
 * Integer snippet:   push [IN_FLAGS]; popfd; <insn>; pushfd; pop [OUT_FLAGS]; ret
 * x87 snippet:       fninit; fldcw [IN_FCW]; fld qword [IN_F2]; fld [IN_F1]; fld [IN_F0]; <insn>;
 *                    fnstsw [OUT_SW]; fstp qword [OUT_F0..]; ...; ret
 */
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { Program } from "../../recomp/src/cfg.ts";
import { loadGhidraHints } from "../../recomp/src/hints.ts";
import { PeImage } from "../../recomp/src/pe.ts";
import { Code, FlowControl, Mnemonic, Register, type Insn } from "../../recomp/src/x86.ts";

export const SNIPPET_BASE = 0x10000000;
export const IO = 0x2ff00000;
export const IO_IN_FLAGS = IO + 0x00;
export const IO_OUT_FLAGS = IO + 0x04;
export const IO_IN_FCW = IO + 0x08;
export const IO_OUT_SW = IO + 0x0c;
export const IO_IN_F = IO + 0x10; // 5 x f64 (ends at 0x38)
export const IO_OUT_F = IO + 0x40; // up to 7 x f64

const [exePath, hintsPath, outDir] = process.argv.slice(2);
const pe = new PeImage(new Uint8Array(readFileSync(exePath)));
const prog = new Program(pe, await loadGhidraHints(hintsPath));
prog.discover();

const X87_PREFIX = 0xd8; // escape opcodes d8..df
function isX87(i: Insn): boolean {
  const b = pe.u8(i.ip);
  return (b >= 0xd8 && b <= 0xdf) || (b === 0x9b && pe.u8(i.ip + 1) >= 0xd8);
}
function isSimd(i: Insn): boolean {
  return i.ops.some((o) => o.kind === "reg" && ((o.reg >= Register.MM0 && o.reg <= Register.MM7) || (o.reg >= Register.XMM0 && o.reg <= Register.XMM31)));
}
const EXCLUDE = new Set<Mnemonic>([Mnemonic.Cpuid, Mnemonic.Int3, Mnemonic.Hlt, Mnemonic.Rdtsc, Mnemonic.Wait, Mnemonic.Nop,
  Mnemonic.Stmxcsr, Mnemonic.Ldmxcsr, Mnemonic.Emms, Mnemonic.Femms, Mnemonic.Fnsave, Mnemonic.Frstor, Mnemonic.Fnstenv, Mnemonic.Fldenv,
  Mnemonic.Enter, Mnemonic.Leave, Mnemonic.Pushad, Mnemonic.Popad]);

// Collect candidate instructions: up to 4 distinct encodings per iced Code.
const byCode = new Map<Code, Insn[]>();
for (const f of prog.funcs.values()) {
  if (f.speculative) continue;
  for (const b of f.blocks.values())
    for (const i of b.insns) {
      if (i.flow !== FlowControl.Next || i.invalid || EXCLUDE.has(i.mnemonic) || isSimd(i)) continue;
      if (i.stackIncrement !== 0) continue; // push/pop/leave would unbalance the snippet's ret
      if (i.mem && (i.mem.seg === Register.GS || i.mem.seg === Register.ES && i.ops.every((o) => o.kind !== "string"))) continue;
      if (i.ops.some((o) => o.kind === "reg" && (o.reg === Register.ESP || o.reg === Register.SP)) && i.mnemonic !== Mnemonic.Push) continue;
      const list = byCode.get(i.code) ?? [];
      if (list.length < 4 && !list.some((x) => x.text === i.text)) list.push(i);
      byCode.set(i.code, list);
    }
}

const code: number[] = [];
const manifest: unknown[] = [];
const u32le = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
for (const insns of byCode.values()) {
  for (const i of insns) {
    while (code.length % 16) code.push(0xcc);
    const addr = SNIPPET_BASE + 0x1000 + code.length;
    const bytes = Array.from(pe.view(i.ip, i.len));
    const x87 = isX87(i);
    if (x87) {
      code.push(0xdb, 0xe3); // fninit
      code.push(0xd9, 0x2d, ...u32le(IO_IN_FCW)); // fldcw [IN_FCW]
      for (const k of [4, 3, 2, 1, 0]) code.push(0xdd, 0x05, ...u32le(IO_IN_F + k * 8)); // fld qword
      code.push(...bytes);
      code.push(0xdd, 0x3d, ...u32le(IO_OUT_SW)); // fnstsw [OUT_SW]
      const depth = 5 - i.fpuTopIncrement;
      for (let k = 0; k < depth; k++) code.push(0xdd, 0x1d, ...u32le(IO_OUT_F + k * 8)); // fstp qword
    } else {
      code.push(0xff, 0x35, ...u32le(IO_IN_FLAGS), 0x9d); // push [IN_FLAGS]; popfd
      code.push(...bytes);
      code.push(0x9c, 0x8f, 0x05, ...u32le(IO_OUT_FLAGS)); // pushfd; pop [OUT_FLAGS]
    }
    code.push(0xc3);
    const m = i.mem;
    manifest.push({
      addr,
      origIp: i.ip,
      text: i.text,
      mnemonic: Mnemonic[i.mnemonic],
      x87,
      fpuInc: i.fpuTopIncrement,
      flagsModified: i.rflagsModified,
      flagsUndefined: i.rflagsUndefined,
      flagsRead: i.rflagsRead,
      mem: m ? { base: Register[m.base], index: Register[m.index], scale: m.scale, disp: m.disp, size: m.size, seg: Register[m.seg] } : null,
      string: i.ops.some((o) => o.kind === "string"),
      regs: i.ops.filter((o) => o.kind === "reg").map((o) => Register[(o as { reg: Register }).reg]),
    });
  }
}

// Minimal PE32: headers + one executable section.
const textSize = code.length;
const align = (v: number, a: number) => Math.ceil(v / a) * a;
const fileAlign = 0x200;
const sectAlign = 0x1000;
const hdr = new Uint8Array(0x400);
const dv = new DataView(hdr.buffer);
dv.setUint16(0, 0x5a4d, true);
dv.setUint32(0x3c, 0x80, true);
dv.setUint32(0x80, 0x4550, true);
dv.setUint16(0x84, 0x14c, true); // machine
dv.setUint16(0x86, 1, true); // sections
dv.setUint16(0x94, 0xe0, true); // optional header size
dv.setUint16(0x96, 0x10f, true);
const opt = 0x98;
dv.setUint16(opt, 0x10b, true);
dv.setUint32(opt + 16, 0x1000, true); // entry rva
dv.setUint32(opt + 28, SNIPPET_BASE, true);
dv.setUint32(opt + 32, sectAlign, true);
dv.setUint32(opt + 36, fileAlign, true);
dv.setUint32(opt + 56, 0x1000 + align(textSize, sectAlign), true); // size of image
dv.setUint32(opt + 60, 0x400, true); // size of headers
dv.setUint16(opt + 68, 2, true); // GUI subsystem
dv.setUint32(opt + 72, 0x100000, true);
dv.setUint32(opt + 76, 0x1000, true);
dv.setUint32(opt + 92, 16, true); // number of data directories
const sec = opt + 0xe0;
".text\0\0\0".split("").forEach((ch, k) => (hdr[sec + k] = ch.charCodeAt(0)));
dv.setUint32(sec + 8, textSize, true);
dv.setUint32(sec + 12, 0x1000, true);
dv.setUint32(sec + 16, align(textSize, fileAlign), true);
dv.setUint32(sec + 20, 0x400, true);
dv.setUint32(sec + 36, 0x60000020, true);
const file = new Uint8Array(0x400 + align(textSize, fileAlign));
file.set(hdr, 0);
file.set(code, 0x400);

mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, "snippets.exe"), file);
writeFileSync(join(outDir, "manifest.json"), JSON.stringify(manifest));
writeFileSync(
  join(outDir, "hints.json"),
  JSON.stringify({ functions: manifest.map((m) => ({ entry: (m as { addr: number }).addr, name: "", thunk: false, noreturn: false, ranges: [] })), computedJumps: [] }),
);
console.log(`${manifest.length} snippets from ${byCode.size} instruction forms; ${textSize} bytes of code`);
void X87_PREFIX;
