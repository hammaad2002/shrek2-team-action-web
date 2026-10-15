/**
 * Installs Shrek 2 Team Action from the player's own disc image: reads the ISO, the MSI
 * database and the LZX cabinet, and writes the INSTALLDIR tree (e.g. `Game/Chars/...`) to a sink.
 */
import { Cabinet } from "./cab.ts";
import { IsoImage } from "./iso9660.ts";
import type { ByteSource, FileSink } from "./io.ts";
import { md5, hex } from "./md5.ts";
import { MsiDatabase, readLayout, type MsiFile, type MsiLayout } from "./msi.ts";

export const PRODUCT_NAME = "Shrek 2 Team Action";

/**
 * Files replaced on the disc after the MSI was built (they are stored uncompressed, newer
 * timestamps), so their MsiFileHash describes an older build. MD5s of known retail copies.
 */
const LATE_MASTERED_MD5: Record<string, string[]> = {
  "Game/Shrek 2.exe": ["c3bdca1f047e722e72afaa899841ed5c"], // EN retail, PE timestamp 2004-10-07
};

export interface InstallProgress {
  phase: "scan" | "extract" | "copy" | "done";
  done: number;
  total: number;
  path?: string;
}

export interface InstallOptions {
  /** Return false to skip a file (paths relative to INSTALLDIR). Default: everything. */
  include?: (installPath: string) => boolean;
  verify?: boolean;
  onProgress?: (p: InstallProgress) => void;
}

export interface InstallReport {
  volumeId: string;
  written: number;
  bytes: number;
  skipped: number;
  verified: number;
  mismatches: string[];
  registry: MsiLayout["registry"];
}

export function openDisc(iso: ByteSource): { image: IsoImage; db: MsiDatabase; layout: MsiLayout } {
  const image = new IsoImage(iso);
  for (const e of image.list("")) {
    if (e.isDir || !e.name.toLowerCase().endsWith(".msi")) continue;
    const db = new MsiDatabase(image.open(e.name).read(0, e.size));
    const layout = readLayout(db);
    if (layout.productName === PRODUCT_NAME) return { image, db, layout };
  }
  throw new Error(`no ${PRODUCT_NAME} installer database found on this disc (volume ${image.volumeId})`);
}

export async function install(iso: ByteSource, sink: FileSink, opts: InstallOptions = {}): Promise<InstallReport> {
  const { image, layout } = openDisc(iso);
  const include = opts.include ?? (() => true);
  const verify = opts.verify ?? true;
  const report: InstallReport = { volumeId: image.volumeId, written: 0, bytes: 0, skipped: 0, verified: 0, mismatches: [], registry: layout.registry };
  const wanted = layout.files.filter((f) => f.installPath !== null && include(f.installPath));
  const total = wanted.length;
  const bySeq = new Map(layout.files.map((f) => [f.sequence, f]));

  const emit = async (f: MsiFile, data: Uint8Array) => {
    if (data.length !== f.size) report.mismatches.push(`${f.installPath}: size ${data.length} != ${f.size}`);
    else if (verify && f.md5) {
      const got = hex(md5(data));
      if (got === hex(f.md5) || LATE_MASTERED_MD5[f.installPath!]?.includes(got)) report.verified++;
      else report.mismatches.push(`${f.installPath}: MD5 mismatch (${got})`);
    }
    await sink.writeFile(f.installPath!, data);
    report.written++;
    report.bytes += data.length;
    opts.onProgress?.({ phase: f.compressed ? "extract" : "copy", done: report.written, total, path: f.installPath! });
  };

  // Compressed files: one cabinet per media entry; CAB file names are MSI File keys, and
  // the i-th file of the cabinet has sequence (previous media's LastSequence + i + 1).
  let prevLast = 0;
  for (const m of [...layout.media].sort((a, b) => a.diskId - b.diskId)) {
    if (m.cabinet) {
      const cabName = m.cabinet.replace(/^#/, "");
      const cab = new Cabinet(image.open(cabName));
      const byKey = new Map(layout.files.map((f) => [f.key, f]));
      const files = cab.files.map((cf, i) => ({ cf, f: byKey.get(cf.name) ?? bySeq.get(prevLast + i + 1) }));
      for (const folder of cab.folders) {
        const members = files.filter((x) => x.cf.folder === folder.index && x.f && x.f.installPath !== null && include(x.f.installPath));
        if (members.length === 0) continue;
        const data = folder.index >= 0 ? cab.extractFolder(folder.index) : new Uint8Array(0);
        for (const { cf, f } of members) await emit(f!, data.subarray(cf.offsetInFolder, cf.offsetInFolder + cf.size));
      }
    }
    prevLast = m.lastSequence;
  }

  // Uncompressed files are stored loose on the disc at their MSI source path.
  for (const f of wanted.filter((x) => !x.compressed)) {
    const e = image.lookup(f.sourcePath);
    if (!e || e.isDir) {
      report.mismatches.push(`${f.installPath}: source not found on disc at ${f.sourcePath}`);
      continue;
    }
    await emit(f, image.open(f.sourcePath).read(0, e.size));
  }
  report.skipped = layout.files.length - report.written;
  opts.onProgress?.({ phase: "done", done: report.written, total });
  return report;
}
