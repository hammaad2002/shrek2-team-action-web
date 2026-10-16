/// <reference lib="webworker" />
/**
 * Installs the game from the player's disc image into OPFS (`/shrek2/...`), off the main thread.
 * Messages in: { file: File }. Messages out: progress / done / error.
 */
import { install } from "../../installer/src/install.ts";
import { BlobSource } from "../../installer/src/io.ts";
import { INSTALL_DIR, INSTALLING_MARKER } from "./cache.ts";
import { OpfsSink } from "./opfs-sink.ts";


self.onmessage = async (e: MessageEvent<{ file: Blob }>) => {
  try {
    const opfs = await navigator.storage.getDirectory();
    // Start from a clean tree but keep saves and settings from a previous install.
    const root = await opfs.getDirectoryHandle(INSTALL_DIR, { create: true });
    // Present until the last file is written: an interrupted install is not mistaken for a
    // finished one (see installed() in main.ts).
    await root.getFileHandle(INSTALLING_MARKER, { create: true });
    const sink = new OpfsSink(root);
    const t0 = performance.now();
    let last = 0;
    const report = await install(new BlobSource(e.data.file), sink, {
      onProgress: (p) => {
        const now = performance.now();
        if (now - last > 100 || p.phase === "done") {
          last = now;
          postMessage({ type: "progress", done: p.done, total: p.total, path: p.path ?? "" });
        }
      },
    });
    await sink.mkdir("Game/Save");
    await root.removeEntry(INSTALLING_MARKER);
    postMessage({
      type: "done",
      seconds: (performance.now() - t0) / 1000,
      written: report.written,
      bytes: report.bytes,
      verified: report.verified,
      mismatches: report.mismatches,
    });
  } catch (err) {
    postMessage({ type: "error", message: err instanceof Error ? `${err.message}\n${err.stack ?? ""}` : String(err) });
  }
};
