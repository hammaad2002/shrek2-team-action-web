/**
 * LZX decompressor for the Microsoft Cabinet variant (one instance per CAB folder).
 *
 * Each CFDATA block carries one 32 KiB output frame whose compressed bits start on a fresh
 * 16-bit boundary; Huffman tree lengths, repeated offsets and the sliding window persist
 * across frames. Verified byte-for-byte against cabextract (see tests/installer).
 */

const NUM_CHARS = 256;
const PRETREE_NUM = 20;
const ALIGNED_NUM = 8;
const LENGTH_NUM = 249;
const MIN_MATCH = 2;
const BLOCK_VERBATIM = 1;
const BLOCK_ALIGNED = 2;
const BLOCK_UNCOMPRESSED = 3;
const OVERFLOW = 0xffffffff;

const POSITION_SLOTS: Record<number, number> = { 15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50 };

const EXTRA_BITS = new Uint8Array(52);
const POSITION_BASE = new Uint32Array(52);
{
  let j = 0;
  for (let i = 0; i < 52; i += 2) {
    EXTRA_BITS[i] = j;
    EXTRA_BITS[i + 1] = j;
    if (i !== 0 && j < 17) j++;
  }
  let base = 0;
  for (let i = 0; i < 52; i++) {
    POSITION_BASE[i] = base;
    base += 1 << EXTRA_BITS[i];
  }
}

/** Canonical Huffman decoding table: direct lookup for short codes, canonical search for long ones. */
class Huffman {
  readonly lens: Uint8Array;
  readonly tableBits: number;
  readonly table: Uint32Array;
  readonly count = new Int32Array(17);
  readonly first = new Int32Array(17);
  readonly offset = new Int32Array(18);
  readonly sorted: Uint16Array;
  empty = true;

  constructor(numSymbols: number, tableBits: number) {
    this.lens = new Uint8Array(numSymbols);
    this.tableBits = tableBits;
    this.table = new Uint32Array(1 << tableBits);
    this.sorted = new Uint16Array(numSymbols);
  }

  build(): void {
    const { lens, count, first, offset, sorted, table, tableBits: T } = this;
    count.fill(0);
    for (let s = 0; s < lens.length; s++) count[lens[s]]++;
    count[0] = 0;
    this.empty = count.every((c) => c === 0);
    table.fill(0);
    if (this.empty) return;
    let code = 0;
    let kraft = 0;
    for (let len = 1; len <= 16; len++) {
      code = (code + count[len - 1]) << 1;
      first[len] = code;
      kraft += count[len] << (16 - len);
    }
    if (kraft !== 1 << 16) throw new Error(`LZX: incomplete or over-subscribed Huffman table (${kraft})`);
    offset[1] = 0;
    for (let len = 1; len <= 16; len++) offset[len + 1] = offset[len] + count[len];
    const next = offset.slice();
    for (let s = 0; s < lens.length; s++) if (lens[s]) sorted[next[lens[s]]++] = s;
    for (let len = 1; len <= 16; len++) {
      for (let k = 0; k < count[len]; k++) {
        const c = first[len] + k;
        const sym = sorted[offset[len] + k];
        if (len <= T) {
          const base = c << (T - len);
          const fill = 1 << (T - len);
          const entry = (len << 16) | sym;
          for (let i = 0; i < fill; i++) table[base + i] = entry;
        } else {
          table[c >>> (len - T)] = OVERFLOW;
        }
      }
    }
  }
}

class BitReader {
  buf: Uint8Array = new Uint8Array(0);
  pos = 0;
  end = 0;
  bitbuf = 0;
  bitsLeft = 0;

  reset(buf: Uint8Array): void {
    this.buf = buf;
    this.pos = 0;
    this.end = buf.length;
    this.bitbuf = 0;
    this.bitsLeft = 0;
  }

  ensure(n: number): void {
    while (this.bitsLeft < n) {
      const p = this.pos;
      const lo = p < this.end ? this.buf[p] : 0;
      const hi = p + 1 < this.end ? this.buf[p + 1] : 0;
      if (p > this.end + 4) throw new Error("LZX: read past end of compressed block");
      this.pos = p + 2;
      this.bitbuf = (this.bitbuf | (((hi << 8) | lo) << (16 - this.bitsLeft))) >>> 0;
      this.bitsLeft += 16;
    }
  }

  readBits(n: number): number {
    if (n === 0) return 0;
    this.ensure(n);
    const v = this.bitbuf >>> (32 - n);
    this.bitbuf = (this.bitbuf << n) >>> 0;
    this.bitsLeft -= n;
    return v;
  }

  decode(h: Huffman): number {
    if (h.empty) throw new Error("LZX: decode from empty Huffman table");
    this.ensure(16);
    const e = h.table[this.bitbuf >>> (32 - h.tableBits)];
    if (e !== OVERFLOW && e !== 0) {
      const len = e >>> 16;
      this.bitbuf = (this.bitbuf << len) >>> 0;
      this.bitsLeft -= len;
      return e & 0xffff;
    }
    const peek = this.bitbuf >>> 16;
    for (let len = h.tableBits + 1; len <= 16; len++) {
      const c = peek >>> (16 - len);
      const idx = c - h.first[len];
      if (idx >= 0 && idx < h.count[len]) {
        this.bitbuf = (this.bitbuf << len) >>> 0;
        this.bitsLeft -= len;
        return h.sorted[h.offset[len] + idx];
      }
    }
    throw new Error("LZX: invalid Huffman code");
  }

  /** Discard buffered bits up to the next 16-bit boundary (always consuming at least one bit). */
  alignForRawData(): void {
    if (this.bitsLeft === 0) this.ensure(16);
    this.bitsLeft = 0;
    this.bitbuf = 0;
  }

  rawByte(): number {
    if (this.pos >= this.end) throw new Error("LZX: uncompressed block data truncated");
    return this.buf[this.pos++];
  }
}

export class LzxDecoder {
  private readonly windowSize: number;
  private readonly window: Uint8Array;
  private readonly mainTree: Huffman;
  private readonly lengthTree = new Huffman(LENGTH_NUM, 12);
  private readonly alignedTree = new Huffman(ALIGNED_NUM, 7);
  private readonly preTree = new Huffman(PRETREE_NUM, 6);
  private readonly br = new BitReader();
  private windowPos = 0;
  private r0 = 1;
  private r1 = 1;
  private r2 = 1;
  private headerRead = false;
  private intelFileSize = 0;
  private intelStarted = false;
  private intelCurPos = 0;
  private frameCount = 0;
  private blockType = 0;
  private blockLength = 0;
  private blockRemaining = 0;

  constructor(windowBits: number) {
    const slots = POSITION_SLOTS[windowBits];
    if (!slots) throw new Error(`LZX: unsupported window size 2^${windowBits}`);
    this.windowSize = 1 << windowBits;
    this.window = new Uint8Array(this.windowSize);
    this.mainTree = new Huffman(NUM_CHARS + slots * 8, 12);
  }

  /** Decode one CFDATA block into `outSize` (<= 32768) bytes. Returns a fresh array. */
  decodeFrame(input: Uint8Array, outSize: number): Uint8Array {
    const br = this.br;
    br.reset(input);
    if (!this.headerRead) {
      this.intelFileSize = br.readBits(1) ? ((br.readBits(16) << 16) | br.readBits(16)) | 0 : 0;
      this.headerRead = true;
    }
    if (this.windowPos === this.windowSize) this.windowPos = 0;
    const frameStart = this.windowPos;
    if (frameStart + outSize > this.windowSize) throw new Error("LZX: frame crosses window end");
    const win = this.window;
    const mask = this.windowSize - 1;
    let pos = this.windowPos;
    let todo = outSize;

    while (todo > 0) {
      if (this.blockRemaining === 0) this.readBlockHeader();
      let run = Math.min(this.blockRemaining, todo);
      todo -= run;
      this.blockRemaining -= run;

      if (this.blockType === BLOCK_UNCOMPRESSED) {
        for (; run > 0; run--) win[pos++] = br.rawByte();
        continue;
      }
      const aligned = this.blockType === BLOCK_ALIGNED;
      const main = this.mainTree;
      while (run > 0) {
        let sym = br.decode(main);
        if (sym < NUM_CHARS) {
          win[pos++] = sym;
          run--;
          continue;
        }
        sym -= NUM_CHARS;
        let matchLen = sym & 7;
        if (matchLen === 7) matchLen += br.decode(this.lengthTree);
        matchLen += MIN_MATCH;
        const slot = sym >>> 3;
        let off: number;
        if (slot === 0) off = this.r0;
        else if (slot === 1) {
          off = this.r1;
          this.r1 = this.r0;
          this.r0 = off;
        } else if (slot === 2) {
          off = this.r2;
          this.r2 = this.r0;
          this.r0 = off;
        } else {
          const extra = EXTRA_BITS[slot];
          off = POSITION_BASE[slot] - 2;
          if (!aligned) {
            off += br.readBits(extra);
          } else if (extra > 3) {
            off += br.readBits(extra - 3) << 3;
            off += br.decode(this.alignedTree);
          } else if (extra === 3) {
            off += br.decode(this.alignedTree);
          } else if (extra > 0) {
            off += br.readBits(extra);
          } else {
            off = 1;
          }
          this.r2 = this.r1;
          this.r1 = this.r0;
          this.r0 = off;
        }
        if (matchLen > run) throw new Error("LZX: match overruns frame/block");
        let src = (pos - off) & mask;
        for (let i = 0; i < matchLen; i++) {
          win[pos++] = win[src];
          src = (src + 1) & mask;
        }
        run -= matchLen;
      }
    }

    this.windowPos = pos;
    const out = win.slice(frameStart, frameStart + outSize);
    if (this.intelStarted && this.intelFileSize !== 0 && this.frameCount < 32768 && outSize > 10) {
      this.undoE8(out);
    }
    this.intelCurPos += outSize;
    this.frameCount++;
    return out;
  }

  private readBlockHeader(): void {
    const br = this.br;
    if (this.blockType === BLOCK_UNCOMPRESSED) {
      if (this.blockLength & 1) br.rawByte(); // pad to even length
      br.bitbuf = 0;
      br.bitsLeft = 0;
    }
    this.blockType = br.readBits(3);
    const hi = br.readBits(16);
    const lo = br.readBits(8);
    this.blockLength = this.blockRemaining = (hi << 8) | lo;
    switch (this.blockType) {
      case BLOCK_ALIGNED:
        for (let i = 0; i < ALIGNED_NUM; i++) this.alignedTree.lens[i] = br.readBits(3);
        this.alignedTree.build();
      // falls through: aligned blocks continue with the same trees as verbatim ones
      case BLOCK_VERBATIM:
        this.readLengths(this.mainTree.lens, 0, NUM_CHARS);
        this.readLengths(this.mainTree.lens, NUM_CHARS, this.mainTree.lens.length);
        this.mainTree.build();
        if (this.mainTree.lens[0xe8] !== 0) this.intelStarted = true;
        this.readLengths(this.lengthTree.lens, 0, LENGTH_NUM);
        this.lengthTree.build();
        break;
      case BLOCK_UNCOMPRESSED: {
        this.intelStarted = true;
        br.alignForRawData();
        const r = new Uint8Array(12);
        for (let i = 0; i < 12; i++) r[i] = br.rawByte();
        const dv = new DataView(r.buffer);
        this.r0 = dv.getUint32(0, true);
        this.r1 = dv.getUint32(4, true);
        this.r2 = dv.getUint32(8, true);
        break;
      }
      default:
        throw new Error(`LZX: bad block type ${this.blockType}`);
    }
  }

  /** Read delta-coded code lengths lens[first..last) using a freshly transmitted pretree. */
  private readLengths(lens: Uint8Array, first: number, last: number): void {
    const br = this.br;
    const pre = this.preTree;
    for (let i = 0; i < PRETREE_NUM; i++) pre.lens[i] = br.readBits(4);
    pre.build();
    let x = first;
    while (x < last) {
      const z = br.decode(pre);
      if (z === 17) {
        let n = br.readBits(4) + 4;
        if (x + n > last) throw new Error("LZX: code length run overflows table");
        while (n-- > 0) lens[x++] = 0;
      } else if (z === 18) {
        let n = br.readBits(5) + 20;
        if (x + n > last) throw new Error("LZX: code length run overflows table");
        while (n-- > 0) lens[x++] = 0;
      } else if (z === 19) {
        let n = br.readBits(1) + 4;
        if (x + n > last) throw new Error("LZX: code length run overflows table");
        let v = lens[x] - br.decode(pre);
        if (v < 0) v += 17;
        while (n-- > 0) lens[x++] = v;
      } else {
        let v = lens[x] - z;
        if (v < 0) v += 17;
        lens[x++] = v;
      }
    }
  }

  /** Reverse the encoder's x86 CALL (E8) absolute-address translation on an output frame. */
  private undoE8(b: Uint8Array): void {
    const size = this.intelFileSize;
    let cur = this.intelCurPos;
    const end = b.length - 10;
    let i = 0;
    while (i < end) {
      if (b[i++] !== 0xe8) {
        cur++;
        continue;
      }
      const abs = b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24);
      if (abs >= -cur && abs < size) {
        const rel = abs >= 0 ? abs - cur : abs + size;
        b[i] = rel & 0xff;
        b[i + 1] = (rel >>> 8) & 0xff;
        b[i + 2] = (rel >>> 16) & 0xff;
        b[i + 3] = (rel >>> 24) & 0xff;
      }
      i += 4;
      cur += 5;
    }
  }
}
