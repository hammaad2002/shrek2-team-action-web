/// <reference lib="webworker" />
// Startup check: the engine's file system needs OPFS sync access handles in workers, which
// private windows refuse (Safari) or lack (Firefox). Posts "ok" or the error message.
self.onmessage = async () => {
  try {
    const dir = await navigator.storage.getDirectory();
    const fh = await dir.getFileHandle(".probe", { create: true });
    const h = await (fh as unknown as { createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle> }).createSyncAccessHandle();
    h.write(new Uint8Array(1), { at: 0 });
    h.close();
    await dir.removeEntry(".probe");
    postMessage("ok");
  } catch (err) {
    postMessage(err instanceof Error ? `${err.name}: ${err.message}` : String(err));
  }
};
