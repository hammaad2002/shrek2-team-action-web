/**
 * FileSink writing into the Origin Private File System from a dedicated worker
 * (FileSystemSyncAccessHandle is only available there).
 */
import type { FileSink } from "../../installer/src/io.ts";

interface SyncHandle {
  truncate(size: number): void;
  write(data: Uint8Array, opts?: { at?: number }): number;
  close(): void;
}

export class OpfsSink implements FileSink {
  private readonly root: FileSystemDirectoryHandle;
  private readonly dirs = new Map<string, Promise<FileSystemDirectoryHandle>>();

  constructor(root: FileSystemDirectoryHandle) {
    this.root = root;
  }

  private dir(path: string): Promise<FileSystemDirectoryHandle> {
    if (path === "") return Promise.resolve(this.root);
    let p = this.dirs.get(path);
    if (!p) {
      const cut = path.lastIndexOf("/");
      const parent = cut < 0 ? "" : path.slice(0, cut);
      const name = cut < 0 ? path : path.slice(cut + 1);
      p = this.dir(parent).then((d) => d.getDirectoryHandle(name, { create: true }));
      this.dirs.set(path, p);
    }
    return p;
  }

  async writeFile(path: string, data: Uint8Array): Promise<void> {
    const cut = path.lastIndexOf("/");
    const dir = await this.dir(cut < 0 ? "" : path.slice(0, cut));
    const fh = await dir.getFileHandle(path.slice(cut + 1), { create: true });
    const h = (await (fh as unknown as { createSyncAccessHandle(): Promise<SyncHandle> }).createSyncAccessHandle()) as SyncHandle;
    // No flush(): it is an fsync per file (10 ms each in Chrome, 45 ms in Firefox, over 4,088
    // files). Closing the handle hands the data to the OS; only an OS crash mid-install could
    // lose it, and the exe, written last, marks a finished install.
    try {
      h.truncate(0);
      let done = 0;
      while (done < data.length) done += h.write(data.subarray(done), { at: done });
    } finally {
      h.close();
    }
  }

  /** Create an (empty) directory, e.g. the game's Save folder. */
  async mkdir(path: string): Promise<void> {
    await this.dir(path);
  }
}
