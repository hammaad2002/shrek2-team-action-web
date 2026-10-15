/**
 * Minimal ISO 9660 reader with Joliet support (read-only, single-extent files).
 */
import { type ByteSource, SliceSource, latin1, u32 } from "./io.ts";

const SECTOR = 2048;

export interface IsoEntry {
  name: string;
  isDir: boolean;
  /** Byte offset of the file data within the image. */
  offset: number;
  size: number;
}

export class IsoImage {
  readonly volumeId: string;
  readonly joliet: boolean;
  private readonly src: ByteSource;
  private readonly root: IsoEntry;
  private readonly dirCache = new Map<number, IsoEntry[]>();

  constructor(src: ByteSource) {
    this.src = src;
    let pvd: Uint8Array | undefined;
    let svd: Uint8Array | undefined;
    for (let sector = 16; sector < 64; sector++) {
      const vd = src.read(sector * SECTOR, SECTOR);
      if (latin1(vd, 1, 6) !== "CD001") throw new Error(`not an ISO 9660 image (sector ${sector})`);
      const type = vd[0];
      if (type === 255) break;
      if (type === 1 && !pvd) pvd = vd;
      // Joliet: supplementary descriptor with a UCS-2 escape sequence (%/@, %/C or %/E).
      if (type === 2 && vd[88] === 0x25 && vd[89] === 0x2f && [0x40, 0x43, 0x45].includes(vd[90])) svd = vd;
    }
    if (!pvd) throw new Error("ISO 9660 primary volume descriptor not found");
    this.volumeId = latin1(pvd, 40, 72).trim();
    this.joliet = svd !== undefined;
    const vd = svd ?? pvd;
    this.root = this.parseRecord(vd, 156)!;
  }

  /** List a directory by slash-separated path ("" = root). Case-insensitive. */
  list(path = ""): IsoEntry[] {
    const dir = path === "" ? this.root : this.lookup(path);
    if (!dir || !dir.isDir) throw new Error(`not a directory: ${path}`);
    return this.readDir(dir);
  }

  /** Find an entry by slash-separated path, case-insensitively. */
  lookup(path: string): IsoEntry | undefined {
    let cur = this.root;
    for (const part of path.split(/[\\/]+/).filter(Boolean)) {
      if (!cur.isDir) return undefined;
      const want = part.toLowerCase();
      const next = this.readDir(cur).find((e) => e.name.toLowerCase() === want);
      if (!next) return undefined;
      cur = next;
    }
    return cur;
  }

  open(path: string): ByteSource {
    const e = this.lookup(path);
    if (!e || e.isDir) throw new Error(`file not found in ISO: ${path}`);
    return new SliceSource(this.src, e.offset, e.size);
  }

  /** Depth-first walk yielding [path, entry] for every file. */
  *walk(path = "", dir: IsoEntry = this.root): Generator<[string, IsoEntry]> {
    for (const e of this.readDir(dir)) {
      const p = path ? `${path}/${e.name}` : e.name;
      if (e.isDir) yield* this.walk(p, e);
      else yield [p, e];
    }
  }

  private readDir(dir: IsoEntry): IsoEntry[] {
    const cached = this.dirCache.get(dir.offset);
    if (cached) return cached;
    const data = this.src.read(dir.offset, dir.size);
    const out: IsoEntry[] = [];
    let o = 0;
    while (o < data.length) {
      const len = data[o];
      if (len === 0) {
        // Records never straddle sectors; zero padding means "continue at next sector".
        o = (Math.floor(o / SECTOR) + 1) * SECTOR;
        continue;
      }
      const e = this.parseRecord(data, o);
      if (e) out.push(e);
      o += len;
    }
    this.dirCache.set(dir.offset, out);
    return out;
  }

  private parseRecord(b: Uint8Array, o: number): IsoEntry | undefined {
    const extent = u32(b, o + 2);
    const size = u32(b, o + 10);
    const flags = b[o + 25];
    const nameLen = b[o + 32];
    const nameStart = o + 33;
    // "\0" and "\1" are the self/parent entries.
    if (nameLen === 1 && (b[nameStart] === 0 || b[nameStart] === 1)) {
      return o === 156 ? { name: "", isDir: true, offset: extent * SECTOR, size } : undefined;
    }
    let name: string;
    if (this.joliet) {
      name = "";
      for (let i = 0; i + 1 < nameLen; i += 2) name += String.fromCharCode((b[nameStart + i] << 8) | b[nameStart + i + 1]);
    } else {
      name = latin1(b, nameStart, nameStart + nameLen);
    }
    name = name.replace(/;\d+$/, "");
    if (!(flags & 2) && name.endsWith(".")) name = name.slice(0, -1);
    if (flags & 0x80) throw new Error(`multi-extent ISO files are not supported: ${name}`);
    return { name, isDir: (flags & 2) !== 0, offset: extent * SECTOR, size };
  }
}
