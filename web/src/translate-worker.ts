/// <reference lib="webworker" />
/**
 * Translates the installed `Shrek 2.exe` (from OPFS) into a WebAssembly module on the player's own
 * machine and caches it in OPFS as `shrek2/cache/<name>`. Messages out: progress / done / error.
 */
import { initIced } from "iced-x86";
import icedWasmUrl from "../../node_modules/iced-x86/iced_x86_bg.wasm?url";
import { translateExe, type ExeMetadata } from "../../recomp/src/translate.ts";
import { cacheName, sha256 } from "./cache.ts";

// Address-level metadata for each supported executable, keyed by SHA-256.
const METADATA = import.meta.glob("../../metadata/*.json", { query: "?url", import: "default", eager: true }) as Record<string, string>;


self.onmessage = async () => {
  try {
    const opfs = await navigator.storage.getDirectory();
    const root = await opfs.getDirectoryHandle("shrek2");
    const game = await root.getDirectoryHandle("Game");
    const exe = new Uint8Array(await (await (await game.getFileHandle("Shrek 2.exe")).getFile()).arrayBuffer());
    const sha = await sha256(exe);
    const metaUrl = METADATA[`../../metadata/${sha}.json`];
    if (!metaUrl) throw new Error(`This version of Shrek 2.exe is not supported yet (SHA-256 ${sha}).`);
    const meta = (await (await fetch(metaUrl)).json()) as ExeMetadata;
    initIced(new Uint8Array(await (await fetch(icedWasmUrl)).arrayBuffer()));
    const t0 = performance.now();
    let last = 0;
    const out = translateExe(exe, meta, (phase, done, total) => {
      const now = performance.now();
      if (now - last > 100) {
        last = now;
        postMessage({ type: "progress", phase, done, total });
      }
    });
    const cache = await root.getDirectoryHandle("cache", { create: true });
    const fh = await cache.getFileHandle(cacheName(sha), { create: true });
    const h = await (fh as unknown as { createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle> }).createSyncAccessHandle();
    h.truncate(0);
    h.write(out.wasm, { at: 0 });
    h.flush();
    h.close();
    postMessage({
      type: "done",
      name: cacheName(sha),
      seconds: (performance.now() - t0) / 1000,
      bytes: out.wasm.length,
      functions: out.functions,
      irreducible: out.irreducible,
    });
  } catch (err) {
    postMessage({ type: "error", message: err instanceof Error ? `${err.message}\n${err.stack ?? ""}` : String(err) });
  }
};
