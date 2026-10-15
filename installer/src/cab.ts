/**
 * Microsoft Cabinet (.cab) reader. Supports uncompressed and LZX folders (MSZIP/Quantum are
 * rejected with a clear error; this game's cabinet is LZX with a 2 MiB window).
 */
import { type ByteSource, cstr, u16, u32 } from "./io.ts";
import { LzxDecoder } from "./lzx.ts";

export interface CabFolder {
  index: number;
  dataOffset: number;
  dataBlocks: number;
  compression: number; // low nibble: 0 none, 1 MSZIP, 2 Quantum, 3 LZX
  windowBits: number;
}

export interface CabFile {
  name: string;
  size: number;
  folder: number;
  offsetInFolder: number;
}

export class Cabinet {
  readonly folders: CabFolder[] = [];
  readonly files: CabFile[] = [];
  private readonly src: ByteSource;
  private readonly dataReserve: number;

  constructor(src: ByteSource) {
    this.src = src;
    const h = src.read(0, Math.min(src.size, 64 * 1024));
    if (h[0] !== 0x4d || h[1] !== 0x53 || h[2] !== 0x43 || h[3] !== 0x46) throw new Error("not a cabinet (MSCF)");
    const coffFiles = u32(h, 16);
    const nFolders = u16(h, 26);
    const nFiles = u16(h, 28);
    const flags = u16(h, 30);
    let o = 36;
    let folderReserve = 0;
    this.dataReserve = 0;
    if (flags & 4) {
      const headerReserve = u16(h, o);
      folderReserve = h[o + 2];
      this.dataReserve = h[o + 3];
      o += 4 + headerReserve;
    }
    if (flags & 1) o = skipStrings(h, o, 2);
    if (flags & 2) o = skipStrings(h, o, 2);
    for (let i = 0; i < nFolders; i++) {
      const type = u16(h, o + 6);
      this.folders.push({ index: i, dataOffset: u32(h, o), dataBlocks: u16(h, o + 4), compression: type & 0xf, windowBits: (type >> 8) & 0x1f });
      o += 8 + folderReserve;
    }
    const fileTable = src.read(coffFiles, Math.min(src.size - coffFiles, nFiles * 300 + 4096));
    let p = 0;
    for (let i = 0; i < nFiles; i++) {
      const folder = u16(fileTable, p + 8);
      if (folder >= 0xfffd) throw new Error("cabinet sets spanning multiple files are not supported");
      const name = cstr(fileTable, p + 16, 1024);
      this.files.push({ name, size: u32(fileTable, p), offsetInFolder: u32(fileTable, p + 4), folder });
      p += 16 + name.length + 1;
    }
  }

  /** Decompress an entire folder into one buffer. */
  extractFolder(index: number): Uint8Array {
    const f = this.folders[index];
    const blocks: { off: number; cb: number; uncomp: number }[] = [];
    let o = f.dataOffset;
    let total = 0;
    for (let i = 0; i < f.dataBlocks; i++) {
      const hdr = this.src.read(o, 8);
      const cb = u16(hdr, 4);
      const uncomp = u16(hdr, 6);
      blocks.push({ off: o + 8 + this.dataReserve, cb, uncomp });
      total += uncomp;
      o += 8 + this.dataReserve + cb;
    }
    const out = new Uint8Array(total);
    let w = 0;
    if (f.compression === 0) {
      for (const b of blocks) {
        out.set(this.src.read(b.off, b.cb), w);
        w += b.cb;
      }
    } else if (f.compression === 3) {
      const lzx = new LzxDecoder(f.windowBits);
      for (const b of blocks) {
        out.set(lzx.decodeFrame(this.src.read(b.off, b.cb), b.uncomp), w);
        w += b.uncomp;
      }
    } else {
      throw new Error(`cabinet folder ${index}: unsupported compression type ${f.compression}`);
    }
    return out;
  }
}

function skipStrings(b: Uint8Array, o: number, n: number): number {
  for (let i = 0; i < n; i++) {
    while (b[o] !== 0) o++;
    o++;
  }
  return o;
}
