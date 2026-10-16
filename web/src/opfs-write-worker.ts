/// <reference lib="webworker" />
// Writes one file into OPFS with a sync access handle (fallback for browsers without
// createWritable on the main thread). Message in: { path: string[], data: Uint8Array }.
self.onmessage = async (e: MessageEvent<{ path: string[]; data: Uint8Array }>) => {
  try {
    const { path, data } = e.data;
    let dir = await navigator.storage.getDirectory();
    for (const part of path.slice(0, -1)) dir = await dir.getDirectoryHandle(part, { create: true });
    const fh = await dir.getFileHandle(path[path.length - 1], { create: true });
    const h = await (fh as unknown as { createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle> }).createSyncAccessHandle();
    h.truncate(0);
    h.write(data, { at: 0 });
    h.flush();
    h.close();
    postMessage({ ok: true });
  } catch (err) {
    postMessage({ ok: false, error: err instanceof Error ? err.message : String(err) });
  }
};
