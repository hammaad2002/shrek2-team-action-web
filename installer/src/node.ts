/** Node.js adapters: file-backed ByteSource and a directory FileSink. */
import { closeSync, fstatSync, mkdirSync, openSync, readSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { checkRange, type ByteSource, type FileSink } from "./io.ts";

export class NodeFileSource implements ByteSource {
  readonly size: number;
  private readonly fd: number;

  constructor(path: string) {
    this.fd = openSync(path, "r");
    this.size = fstatSync(this.fd).size;
  }

  read(offset: number, length: number): Uint8Array {
    checkRange(offset, length, this.size);
    const buf = new Uint8Array(length);
    let done = 0;
    while (done < length) {
      const n = readSync(this.fd, buf, done, length - done, offset + done);
      if (n <= 0) throw new Error(`short read at ${offset + done}`);
      done += n;
    }
    return buf;
  }

  close(): void {
    closeSync(this.fd);
  }
}

export class DirSink implements FileSink {
  private readonly root: string;
  private readonly made = new Set<string>();

  constructor(root: string) {
    this.root = root;
  }

  writeFile(path: string, data: Uint8Array): void {
    const full = join(this.root, ...path.split("/"));
    const dir = dirname(full);
    if (!this.made.has(dir)) {
      mkdirSync(dir, { recursive: true });
      this.made.add(dir);
    }
    writeFileSync(full, data);
  }
}
