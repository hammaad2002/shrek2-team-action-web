/**
 * Windows Installer (MSI) database reader: string pool, schema-driven tables, and the
 * File/Component/Directory/Media/MsiFileHash views the installer needs.
 */
import { CompoundFile, type CfbEntry } from "./cfb.ts";
import { u16 } from "./io.ts";

const MSITYPE_STRING = 0x0800;

type Cell = string | number | null;
export type Row = Record<string, Cell>;

/** MSI encodes stream names into code points 0x3800..0x4840 (two base-64 chars per code point). */
export function decodeStreamName(name: string): string {
  const mime = (x: number) =>
    x < 10 ? String.fromCharCode(48 + x) : x < 36 ? String.fromCharCode(55 + x) : x < 62 ? String.fromCharCode(61 + x) : x === 62 ? "." : "_";
  let out = "";
  for (const ch of name) {
    let c = ch.codePointAt(0)!;
    if (c >= 0x3800 && c < 0x4840) {
      if (c >= 0x4800) out += mime(c - 0x4800);
      else {
        c -= 0x3800;
        out += mime(c & 0x3f) + mime((c >> 6) & 0x3f);
      }
    } else if (c === 0x4840) out += "!";
    else out += ch;
  }
  return out;
}

export class MsiDatabase {
  readonly codepage: number;
  private readonly cfb: CompoundFile;
  private readonly streams = new Map<string, CfbEntry>();
  private readonly strings: (string | null)[];
  private readonly strRefSize: number;
  private readonly schema = new Map<string, { name: string; type: number }[]>();
  private readonly decoder: TextDecoder;

  constructor(bytes: Uint8Array) {
    this.cfb = new CompoundFile(bytes);
    for (const e of this.cfb.streams()) this.streams.set(decodeStreamName(e.name), e);
    const pool = this.stream("!_StringPool");
    const data = this.stream("!_StringData");
    const w = (i: number) => u16(pool, i * 2);
    this.codepage = w(0) | ((w(1) & 0x7fff) << 16);
    this.strRefSize = w(1) & 0x8000 ? 3 : 2;
    this.decoder = new TextDecoder(this.codepage === 65001 ? "utf-8" : "windows-1252");
    const count = pool.length / 4;
    this.strings = [null];
    let off = 0;
    for (let i = 1; i < count; ) {
      let len = w(i * 2);
      const refs = w(i * 2 + 1);
      if (len === 0 && refs === 0) {
        this.strings.push("");
        i++;
        continue;
      }
      if (len === 0) {
        // Long string: length's high word is in the next entry.
        len = (w(i * 2 + 3) << 16) + w(i * 2 + 2);
        i += 2;
      } else i += 1;
      this.strings.push(this.decoder.decode(data.subarray(off, off + len)));
      off += len;
    }
    for (const r of this.readTable("_Columns", [
      { name: "Table", type: MSITYPE_STRING },
      { name: "Number", type: 0x0102 },
      { name: "Name", type: MSITYPE_STRING },
      { name: "Type", type: 0x0102 },
    ])) {
      const t = r.Table as string;
      const cols = this.schema.get(t) ?? [];
      cols[(r.Number as number) - 1] = { name: r.Name as string, type: r.Type as number };
      this.schema.set(t, cols);
    }
  }

  tables(): string[] {
    return [...this.schema.keys()].sort();
  }

  table(name: string): Row[] {
    const cols = this.schema.get(name);
    if (!cols) throw new Error(`MSI table not found: ${name}`);
    if (!this.streams.has("!" + name)) return []; // declared but empty
    return this.readTable(name, cols);
  }

  stream(decodedName: string): Uint8Array {
    const e = this.streams.get(decodedName);
    if (!e) throw new Error(`MSI stream not found: ${decodedName}`);
    return this.cfb.read(e);
  }

  private readTable(name: string, cols: { name: string; type: number }[]): Row[] {
    const raw = this.stream("!" + name);
    const widths = cols.map((c) => (c.type & MSITYPE_STRING ? this.strRefSize : (c.type & 0xff) <= 2 ? 2 : 4));
    const rowSize = widths.reduce((a, b) => a + b, 0);
    const n = Math.floor(raw.length / rowSize);
    const rows: Row[] = Array.from({ length: n }, () => ({}));
    let o = 0;
    cols.forEach((c, ci) => {
      const wdt = widths[ci];
      for (let r = 0; r < n; r++) {
        let v = 0;
        for (let k = wdt - 1; k >= 0; k--) v = v * 256 + raw[o + r * wdt + k];
        let cell: Cell;
        if (c.type & MSITYPE_STRING) cell = v ? (this.strings[v] ?? null) : null;
        else if (v === 0) cell = null;
        else cell = wdt === 2 ? v - 0x8000 : v - 0x80000000;
        rows[r][c.name] = cell;
      }
      o += wdt * n;
    });
    return rows;
  }
}

export interface MsiFile {
  key: string;
  /** Install path relative to INSTALLDIR (forward slashes), or null if outside INSTALLDIR. */
  installPath: string | null;
  /** Path relative to the media root for uncompressed files. */
  sourcePath: string;
  size: number;
  sequence: number;
  compressed: boolean;
  /** MD5 from MsiFileHash (16 bytes) when present. */
  md5: Uint8Array | null;
}

export interface MsiLayout {
  productName: string;
  files: MsiFile[];
  media: { diskId: number; lastSequence: number; cabinet: string | null; volumeLabel: string | null }[];
  registry: { root: number; key: string; name: string | null; value: string | null }[];
}

/** Resolve the install layout of every file, relative to INSTALLDIR. */
export function readLayout(db: MsiDatabase): MsiLayout {
  const dirs = new Map(db.table("Directory").map((r) => [r.Directory as string, r]));
  const longName = (s: string) => (s.includes("|") ? s.split("|")[1] : s);
  const split = (defaultDir: string): [string, string] => {
    const [target, source] = defaultDir.split(":");
    return [longName(target), longName(source ?? target)];
  };
  // inst: path below INSTALLDIR (null when the directory is not inside it);
  // src: path below the media root, used for uncompressed ("loose") source files.
  const memo = new Map<string, { inst: string[] | null; src: string[] }>();
  const resolve = (id: string): { inst: string[] | null; src: string[] } => {
    const hit = memo.get(id);
    if (hit) return hit;
    const r = dirs.get(id);
    if (!r) throw new Error(`unknown MSI directory ${id}`);
    const [target, source] = split(r.DefaultDir as string);
    const parent = r.Directory_Parent as string | null;
    let res: { inst: string[] | null; src: string[] };
    if (!parent || parent === id) {
      res = { inst: null, src: [] }; // TARGETDIR (SourceDir) is the media root
    } else {
      const p = resolve(parent);
      const inst = id === "INSTALLDIR" ? [] : p.inst === null ? null : target === "." ? p.inst : [...p.inst, target];
      res = { inst, src: source === "." ? p.src : [...p.src, source] };
    }
    memo.set(id, res);
    return res;
  };
  const comps = new Map(db.table("Component").map((r) => [r.Component as string, r]));
  const hashes = new Map<string, Uint8Array>();
  for (const r of db.table("MsiFileHash")) {
    const md5 = new Uint8Array(16);
    const dv = new DataView(md5.buffer);
    [r.HashPart1, r.HashPart2, r.HashPart3, r.HashPart4].forEach((v, i) => dv.setInt32(i * 4, (v as number) | 0, true));
    hashes.set(r.File_ as string, md5);
  }
  const files: MsiFile[] = db.table("File").map((f) => {
    const comp = comps.get(f.Component_ as string);
    if (!comp) throw new Error(`file ${f.File} references unknown component`);
    const d = resolve(comp.Directory_ as string);
    const name = longName(f.FileName as string);
    const attrs = (f.Attributes as number | null) ?? 0;
    return {
      key: f.File as string,
      installPath: d.inst === null ? null : [...d.inst, name].join("/"),
      sourcePath: [...d.src, name].join("/"),
      size: f.FileSize as number,
      sequence: f.Sequence as number,
      compressed: (attrs & 0x2000) === 0, // msidbFileAttributesNoncompressed
      md5: hashes.get(f.File as string) ?? null,
    };
  });
  const prop = new Map(db.table("Property").map((r) => [r.Property as string, r.Value as string]));
  return {
    productName: prop.get("ProductName") ?? "",
    files,
    media: db.table("Media").map((m) => ({
      diskId: m.DiskId as number,
      lastSequence: m.LastSequence as number,
      cabinet: m.Cabinet as string | null,
      volumeLabel: m.VolumeLabel as string | null,
    })),
    registry: db.table("Registry").map((r) => ({
      root: r.Root as number,
      key: r.Key as string,
      name: r.Name as string | null,
      value: r.Value as string | null,
    })),
  };
}
