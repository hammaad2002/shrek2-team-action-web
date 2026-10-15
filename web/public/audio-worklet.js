// Plays the engine's audio ring buffer (interleaved stereo float frames in shared WebAssembly
// memory; the engine's feeder thread writes, this processor reads).
class RingPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { buffer, header } = options.processorOptions;
    const u32 = new Uint32Array(buffer);
    const h = header >>> 2;
    this.u32 = u32;
    this.writeIdx = u32[h] >>> 2;
    this.readIdx = u32[h + 1] >>> 2;
    this.frames = u32[h + 2];
    this.data = new Float32Array(buffer, u32[h + 4], this.frames * 2);
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const left = out[0];
    const right = out[1] ?? out[0];
    let r = Atomics.load(this.u32, this.readIdx);
    const w = Atomics.load(this.u32, this.writeIdx);
    const mask = this.frames - 1;
    for (let i = 0; i < left.length; i++) {
      if (r !== w) {
        const s = (r & mask) * 2;
        left[i] = this.data[s];
        right[i] = this.data[s + 1];
        r = (r + 1) >>> 0;
      } else {
        left[i] = 0;
        right[i] = 0;
      }
    }
    Atomics.store(this.u32, this.readIdx, r);
    return true;
  }
}

registerProcessor("ring-player", RingPlayer);
