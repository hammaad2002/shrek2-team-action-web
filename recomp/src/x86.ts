/**
 * x86-32 instruction decoding (iced-x86 under the hood) into plain, immutable records so the
 * rest of the recompiler never touches the decoder's WASM objects.
 */
import {
  Code,
  ConditionCode,
  Decoder,
  DecoderOptions,
  FlowControl,
  MemorySize,
  MemorySizeExt,
  Mnemonic,
  OpKind,
  Register,
} from "iced-x86";

export { Code, ConditionCode, FlowControl, MemorySize, Mnemonic, OpKind, Register };

export interface MemOperand {
  base: Register;
  index: Register;
  scale: number;
  /** Displacement as an unsigned 32-bit value. */
  disp: number;
  /** Effective segment (DS/SS/ES/FS/...), after defaults and overrides. */
  seg: Register;
  /** Access size in bytes (0 for e.g. LEA / unknown). */
  size: number;
  type: MemorySize;
}

export type Operand =
  | { kind: "reg"; reg: Register }
  | { kind: "imm"; value: number } // unsigned 32-bit (sign-extension already applied by the encoding)
  | { kind: "mem"; mem: MemOperand }
  | { kind: "branch"; target: number }
  | { kind: "string" }; // implicit string-instruction memory operand ([esi]/[edi])

export interface Insn {
  ip: number;
  len: number;
  next: number;
  code: Code;
  mnemonic: Mnemonic;
  ops: Operand[];
  flow: FlowControl;
  /** Near branch target (jmp/jcc/call/loop rel), else 0. */
  target: number;
  mem: MemOperand | null;
  rep: boolean;
  repne: boolean;
  lock: boolean;
  cc: ConditionCode;
  /** EFLAGS bits read (iced RflagsBits: OF=1 SF=2 ZF=4 AF=8 CF=16 PF=32 DF=64; C0..C3 for x87). */
  rflagsRead: number;
  /** EFLAGS bits modified in any way (written, set, cleared or left undefined). */
  rflagsModified: number;
  rflagsUndefined: number;
  fpuTopIncrement: number;
  stackIncrement: number;
  invalid: boolean;
  text: string;
}

const u32 = (v: bigint) => Number(BigInt.asUintN(32, v));

export class X86Decoder {
  private readonly cache = new Map<number, Insn>();
  private readonly read: (va: number, len: number) => Uint8Array;

  /** `read` must return the bytes at a virtual address (fewer at the end of the image). */
  constructor(read: (va: number, len: number) => Uint8Array) {
    this.read = read;
  }

  decode(ip: number): Insn {
    const hit = this.cache.get(ip);
    if (hit) return hit;
    const bytes = this.read(ip, 16);
    const d = new Decoder(32, bytes, DecoderOptions.None);
    d.ip = BigInt(ip);
    const i = d.decode();
    const insn = convert(i, ip);
    i.free();
    d.free();
    this.cache.set(ip, insn);
    return insn;
  }

  /** Number of distinct instructions decoded so far. */
  get size(): number {
    return this.cache.size;
  }
}

function convert(i: import("iced-x86").Instruction, ip: number): Insn {
  // Far control transfers never occur in flat Win32 code; treat them as invalid (data, not code).
  const invalid = i.isInvalid || i.code === Code.INVALID || i.isCallFar || i.isJmpFar || i.isCallFarIndirect || i.isJmpFarIndirect;
  const ops: Operand[] = [];
  let mem: MemOperand | null = null;
  let target = 0;
  const opCount = invalid ? 0 : i.opCount;
  for (let k = 0; k < opCount; k++) {
    const kind = i.opKind(k);
    switch (kind) {
      case OpKind.Register:
        ops.push({ kind: "reg", reg: i.opRegister(k) });
        break;
      case OpKind.NearBranch16:
      case OpKind.NearBranch32:
        target = i.nearBranch32;
        ops.push({ kind: "branch", target });
        break;
      case OpKind.Immediate8:
      case OpKind.Immediate8_2nd:
      case OpKind.Immediate16:
      case OpKind.Immediate32:
      case OpKind.Immediate8to16:
      case OpKind.Immediate8to32:
        ops.push({ kind: "imm", value: u32(i.immediate(k)) });
        break;
      case OpKind.Memory: {
        const type = i.memorySize;
        mem = {
          base: i.memoryBase,
          index: i.memoryIndex,
          scale: i.memoryIndexScale,
          disp: u32(i.memoryDisplacement),
          seg: i.memorySegment,
          size: MemorySizeExt.size(type),
          type,
        };
        ops.push({ kind: "mem", mem });
        break;
      }
      case OpKind.MemorySegSI:
      case OpKind.MemorySegESI:
      case OpKind.MemorySegDI:
      case OpKind.MemorySegEDI:
      case OpKind.MemoryESDI:
      case OpKind.MemoryESEDI:
        ops.push({ kind: "string" });
        break;
      default:
        throw new Error(`unsupported operand kind ${OpKind[kind]} at 0x${ip.toString(16)}: ${i.toString()}`);
    }
  }
  return {
    ip,
    len: invalid ? 1 : i.length,
    next: ip + (invalid ? 1 : i.length),
    code: i.code,
    mnemonic: i.mnemonic,
    ops,
    flow: invalid ? FlowControl.Exception : i.flowControl,
    target,
    mem,
    rep: i.hasRepPrefix || i.hasRepePrefix,
    repne: i.hasRepnePrefix,
    lock: i.hasLockPrefix,
    cc: i.conditionCode,
    rflagsRead: i.rflagsRead,
    rflagsModified: i.rflagsModified,
    rflagsUndefined: i.rflagsUndefined,
    fpuTopIncrement: i.fpuTopIncrement,
    stackIncrement: i.stackPointerIncrement,
    invalid,
    text: invalid ? "(bad)" : i.toString(),
  };
}

export function regName(r: Register): string {
  return Register[r];
}
