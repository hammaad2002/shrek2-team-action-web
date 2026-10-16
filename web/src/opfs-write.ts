/**
 * Write a small file into OPFS from the page. Uses FileSystemFileHandle.createWritable when the
 * browser has it on the main thread, otherwise a short-lived worker with a sync access handle
 * (the only write path older Safari versions offer).
 */
export async function writeOpfsFile(path: string[], data: Uint8Array<ArrayBuffer>): Promise<void> {
  let dir = await navigator.storage.getDirectory();
  for (const part of path.slice(0, -1)) dir = await dir.getDirectoryHandle(part, { create: true });
  const fh = await dir.getFileHandle(path[path.length - 1], { create: true });
  const withWritable = fh as FileSystemFileHandle & { createWritable?: () => Promise<FileSystemWritableFileStream> };
  if (typeof withWritable.createWritable === "function") {
    const w = await withWritable.createWritable();
    await w.write(data);
    await w.close();
    return;
  }
  await new Promise<void>((resolve, reject) => {
    const worker = new Worker(new URL("./opfs-write-worker.ts", import.meta.url), { type: "module" });
    worker.onmessage = (e: MessageEvent<{ ok: boolean; error?: string }>) => {
      worker.terminate();
      if (e.data.ok) resolve();
      else reject(new Error(e.data.error));
    };
    worker.onerror = (e) => {
      worker.terminate();
      reject(new Error(e.message || "OPFS write worker failed"));
    };
    worker.postMessage({ path, data });
  });
}

export async function readOpfsFile(path: string[]): Promise<Uint8Array | null> {
  try {
    let dir = await navigator.storage.getDirectory();
    for (const part of path.slice(0, -1)) dir = await dir.getDirectoryHandle(part);
    const f = await (await dir.getFileHandle(path[path.length - 1])).getFile();
    return new Uint8Array(await f.arrayBuffer());
  } catch {
    return null;
  }
}
