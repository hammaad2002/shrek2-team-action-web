/**
 * Save backups. The game keeps up to three slots in Game/Save (save.bin, save.bin_2, save.bin_3)
 * inside this browser's private storage; exporting is the only backup.
 */
import { INSTALL_DIR } from "./cache.ts";
import { writeOpfsFile } from "./opfs-write.ts";
import { makeZip, readZip } from "./zip.ts";

const SAVE_NAME = /^save\.bin(_2|_3)?$/i;
const MAX_SAVE_BYTES = 4 << 20;

async function saveDir(create: boolean): Promise<FileSystemDirectoryHandle> {
  const root = await (await navigator.storage.getDirectory()).getDirectoryHandle(INSTALL_DIR);
  const game = await root.getDirectoryHandle("Game");
  return game.getDirectoryHandle("Save", { create });
}

/** The save files currently stored, by canonical name. */
export async function listSaves(): Promise<{ name: string; data: Uint8Array<ArrayBuffer>; date: Date }[]> {
  let dir: FileSystemDirectoryHandle;
  try {
    dir = await saveDir(false);
  } catch {
    return [];
  }
  const out: { name: string; data: Uint8Array<ArrayBuffer>; date: Date }[] = [];
  for await (const [name, handle] of (dir as unknown as { entries(): AsyncIterable<[string, FileSystemHandle]> }).entries()) {
    if (handle.kind !== "file" || !SAVE_NAME.test(name)) continue;
    const f = await (handle as FileSystemFileHandle).getFile();
    out.push({ name: name.toLowerCase(), data: new Uint8Array(await f.arrayBuffer()), date: new Date(f.lastModified) });
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

/** A ZIP of all save slots, or null when there are none yet. */
export async function exportSaves(): Promise<Blob | null> {
  const saves = await listSaves();
  if (!saves.length) return null;
  return new Blob([makeZip(saves.map((s) => ({ name: `Save/${s.name}`, data: s.data, date: s.date })))], { type: "application/zip" });
}

export interface ImportPlan {
  files: { name: string; data: Uint8Array<ArrayBuffer> }[];
  overwrites: string[];
}

/** Validate chosen files (a backup ZIP or individual save.bin files) before writing anything. */
export async function planImport(files: File[]): Promise<ImportPlan> {
  const found = new Map<string, Uint8Array<ArrayBuffer>>();
  for (const f of files) {
    if (/\.zip$/i.test(f.name)) {
      for (const e of await readZip(new Uint8Array(await f.arrayBuffer()))) {
        const base = e.name.split("/").pop() ?? "";
        if (SAVE_NAME.test(base)) found.set(base.toLowerCase(), e.data);
      }
    } else if (SAVE_NAME.test(f.name)) {
      found.set(f.name.toLowerCase(), new Uint8Array(await f.arrayBuffer()));
    } else {
      throw new Error(`${f.name} is not a Shrek 2 save (expected save.bin, save.bin_2, save.bin_3 or a backup .zip)`);
    }
  }
  if (!found.size) throw new Error("no Shrek 2 save files found in the selection");
  for (const [name, data] of found) {
    if (data.length === 0 || data.length > MAX_SAVE_BYTES) throw new Error(`${name} has an unexpected size (${data.length} bytes)`);
  }
  const existing = new Set((await listSaves()).map((s) => s.name));
  return {
    files: [...found].map(([name, data]) => ({ name, data })),
    overwrites: [...found.keys()].filter((n) => existing.has(n)),
  };
}

export async function applyImport(plan: ImportPlan): Promise<void> {
  for (const f of plan.files) await writeOpfsFile([INSTALL_DIR, "Game", "Save", f.name], f.data);
}
