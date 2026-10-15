/**
 * OLE Compound File Binary (CFB / "structured storage") reader, enough for MSI databases.
 * Loads the whole file into memory (MSI files are small).
 */
import { u16, u32 } from "./io.ts";

const ENDOFCHAIN = 0xfffffffe;
const FREESECT = 0xffffffff;

export interface CfbEntry {
  /** Raw UTF-16 name (MSI stream names use private-use code points; see msi.ts). */
  name: string;
  type: number; // 1 storage, 2 stream, 5 root
  start: number;
  size: number;
  left: number;
  right: number;
  child: number;
}

export class CompoundFile {
  readonly entries: CfbEntry[] = [];
  private readonly b: Uint8Array;
  private readonly sectorSize: number;
  private readonly miniSectorSize: number;
  private readonly miniCutoff: number;
  private readonly fat: Uint32Array;
  private readonly miniFat: Uint32Array;
  private readonly miniStream: Uint8Array;

  constructor(bytes: Uint8Array) {
    this.b = bytes;
    const sig = [0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1];
    if (!sig.every((v, i) => bytes[i] === v)) throw new Error("not an OLE compound file");
    if (u16(bytes, 0x1c) !== 0xfffe) throw new Error("unexpected CFB byte order mark");
    this.sectorSize = 1 << u16(bytes, 0x1e);
    this.miniSectorSize = 1 << u16(bytes, 0x20);
    const numFatSectors = u32(bytes, 0x2c);
    const firstDirSector = u32(bytes, 0x30);
    this.miniCutoff = u32(bytes, 0x38);
    const firstMiniFat = u32(bytes, 0x3c);
    let difatSector = u32(bytes, 0x44);
    const numDifat = u32(bytes, 0x48);

    // Collect FAT sector locations from the header DIFAT and DIFAT chain.
    const fatSectors: number[] = [];
    for (let i = 0; i < 109 && fatSectors.length < numFatSectors; i++) fatSectors.push(u32(bytes, 0x4c + i * 4));
    for (let n = 0; n < numDifat && difatSector !== ENDOFCHAIN && difatSector !== FREESECT; n++) {
      const base = this.sectorOffset(difatSector);
      const per = this.sectorSize / 4 - 1;
      for (let i = 0; i < per && fatSectors.length < numFatSectors; i++) fatSectors.push(u32(bytes, base + i * 4));
      difatSector = u32(bytes, base + per * 4);
    }
    const per = this.sectorSize / 4;
    this.fat = new Uint32Array(fatSectors.length * per);
    fatSectors.forEach((s, k) => {
      const base = this.sectorOffset(s);
      for (let i = 0; i < per; i++) this.fat[k * per + i] = u32(bytes, base + i * 4);
    });

    // Directory entries.
    const dir = this.readChain(firstDirSector, this.fat, this.sectorSize, (s) => this.sectorOffset(s), bytes);
    for (let o = 0; o + 128 <= dir.length; o += 128) {
      const nameLen = u16(dir, o + 0x40);
      let name = "";
      for (let i = 0; i + 2 < nameLen; i += 2) name += String.fromCharCode(u16(dir, o + i));
      this.entries.push({
        name,
        type: dir[o + 0x42],
        left: u32(dir, o + 0x44),
        right: u32(dir, o + 0x48),
        child: u32(dir, o + 0x4c),
        start: u32(dir, o + 0x74),
        size: u32(dir, o + 0x78), // v3 files: high dword is undefined/zero
      });
    }
    const root = this.entries[0];
    if (!root || root.type !== 5) throw new Error("CFB root entry missing");

    const miniFatBytes =
      firstMiniFat === ENDOFCHAIN ? new Uint8Array(0) : this.readChain(firstMiniFat, this.fat, this.sectorSize, (s) => this.sectorOffset(s), bytes);
    this.miniFat = new Uint32Array(miniFatBytes.length / 4);
    for (let i = 0; i < this.miniFat.length; i++) this.miniFat[i] = u32(miniFatBytes, i * 4);
    this.miniStream =
      root.start === ENDOFCHAIN ? new Uint8Array(0) : this.readChain(root.start, this.fat, this.sectorSize, (s) => this.sectorOffset(s), bytes).subarray(0, root.size);
  }

  /** All stream entries directly or indirectly under the root (MSI is flat). */
  streams(): CfbEntry[] {
    return this.entries.filter((e) => e.type === 2);
  }

  read(entry: CfbEntry): Uint8Array {
    if (entry.size < this.miniCutoff) {
      const ms = this.miniStream;
      const data = this.readChain(entry.start, this.miniFat, this.miniSectorSize, (s) => s * this.miniSectorSize, ms);
      return data.subarray(0, entry.size);
    }
    return this.readChain(entry.start, this.fat, this.sectorSize, (s) => this.sectorOffset(s), this.b).subarray(0, entry.size);
  }

  private sectorOffset(sector: number): number {
    return (sector + 1) * this.sectorSize;
  }

  private readChain(
    start: number,
    fat: Uint32Array,
    size: number,
    offsetOf: (s: number) => number,
    bytes: Uint8Array,
  ): Uint8Array {
    const chunks: Uint8Array[] = [];
    let s = start;
    let guard = 0;
    while (s !== ENDOFCHAIN) {
      if (s === FREESECT || s >= fat.length || guard++ > fat.length) throw new Error(`corrupt CFB chain at sector ${s}`);
      const o = offsetOf(s);
      chunks.push(bytes.subarray(o, o + size));
      s = fat[s];
    }
    const out = new Uint8Array(chunks.length * size);
    chunks.forEach((c, i) => out.set(c, i * size));
    return out;
  }
}

