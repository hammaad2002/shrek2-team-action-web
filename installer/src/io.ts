/**
 * Synchronous random-access byte sources and asynchronous file sinks.
 *
 * Sources are synchronous on purpose: in the browser the installer runs in a Web Worker,
 * where `FileReaderSync` (and OPFS sync handles) allow blocking reads, and in Node we use
 * `fs.readSync`. That keeps every parser below simple, sequential code.
 */

export interface ByteSource {
  readonly size: number;
  /** Read exactly `length` bytes at `offset`; throws if the range is out of bounds. */
  read(offset: number, length: number): Uint8Array;
}

export class MemorySource implements ByteSource {
  readonly size: number;
  private readonly bytes: Uint8Array;

  constructor(bytes: Uint8Array) {
    this.bytes = bytes;
    this.size = bytes.length;
  }

  read(offset: number, length: number): Uint8Array {
    checkRange(offset, length, this.size);
    return this.bytes.subarray(offset, offset + length);
  }
}

/** A window [start, start+size) of another source. */
export class SliceSource implements ByteSource {
  readonly size: number;
  private readonly base: ByteSource;
  private readonly start: number;

  constructor(base: ByteSource, start: number, size: number) {
    checkRange(start, size, base.size);
    this.base = base;
    this.start = start;
    this.size = size;
  }

  read(offset: number, length: number): Uint8Array {
    checkRange(offset, length, this.size);
    return this.base.read(this.start + offset, length);
  }
}

/** Browser worker source backed by a `File`/`Blob` via `FileReaderSync`. */
export class BlobSource implements ByteSource {
  readonly size: number;
  private readonly blob: Blob;
  private readonly reader: { readAsArrayBuffer(b: Blob): ArrayBuffer };

  constructor(blob: Blob) {
    const Ctor = (globalThis as unknown as { FileReaderSync?: new () => { readAsArrayBuffer(b: Blob): ArrayBuffer } })
      .FileReaderSync;
    if (!Ctor) throw new Error("BlobSource requires FileReaderSync (run the installer in a Web Worker)");
    this.blob = blob;
    this.size = blob.size;
    this.reader = new Ctor();
  }

  read(offset: number, length: number): Uint8Array {
    checkRange(offset, length, this.size);
    return new Uint8Array(this.reader.readAsArrayBuffer(this.blob.slice(offset, offset + length)));
  }
}

export function checkRange(offset: number, length: number, size: number): void {
  if (!Number.isInteger(offset) || !Number.isInteger(length) || offset < 0 || length < 0 || offset + length > size) {
    throw new RangeError(`read [${offset}, +${length}) outside source of size ${size}`);
  }
}

/** Destination for installed files; paths use forward slashes, relative to the install root. */
export interface FileSink {
  writeFile(path: string, data: Uint8Array): Promise<void> | void;
}

/** Little-endian readers over a Uint8Array. */
export function u16(b: Uint8Array, o: number): number {
  return b[o] | (b[o + 1] << 8);
}

export function u32(b: Uint8Array, o: number): number {
  return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
}

export function i32(b: Uint8Array, o: number): number {
  return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24);
}

export function latin1(b: Uint8Array, start: number, end: number): string {
  let s = "";
  for (let i = start; i < end; i++) s += String.fromCharCode(b[i]);
  return s;
}

/** NUL-terminated string starting at `o` (latin-1). */
export function cstr(b: Uint8Array, o: number, max = b.length - o): string {
  let e = o;
  const lim = Math.min(b.length, o + max);
  while (e < lim && b[e] !== 0) e++;
  return latin1(b, o, e);
}
