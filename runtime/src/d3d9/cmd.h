// Render command stream between the D3D9 HLE (game thread) and the GLES3 executor (render thread:
// the same thread natively, the page's main thread on the web). Commands carry copies of any guest
// data they reference, so the game may reuse its memory as soon as a call returns.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace cmd {

enum Op : uint32_t {
  // Resources (ids are allocated by the recorder).
  OpCreateTexture,     // id, kind(0=2D,1=cube), w, h, levels, format, rt(0/1)
  OpDestroyTexture,    // id
  OpUploadTexture,     // id, face, level, w, h, format, pitch, rows, bytes...
  OpCreateRenderbuffer,// id, w, h, format
  OpDestroyRenderbuffer,
  OpCreateBuffer,      // id, kind(0=vertex,1=index), size
  OpDestroyBuffer,     // id
  OpUploadBuffer,      // id, offset, size, bytes...
  OpCreateShader,      // id, pixel(0/1), ntokens, tokens...
  OpDestroyShader,     // id
  // State.
  OpSetRenderState,    // state, value
  OpSetTextureStageState,  // stage, type, value
  OpSetSamplerState,   // sampler, type, value
  OpSetTexture,        // stage, texture id (0 = none)
  OpSetTransform,      // state, 16 floats
  OpSetViewport,       // x, y, w, h, minz, maxz
  OpSetScissor,        // l, t, r, b
  OpSetMaterial,       // 17 floats
  OpSetLight,          // index, 26 values (D3DLIGHT9)
  OpLightEnable,       // index, enable
  OpSetClipPlane,      // index, 4 floats
  OpSetVertexLayout,   // nelements, then per element: stream, offset, type, usage, usage_index
  OpSetStream,         // stream, buffer id, offset, stride
  OpSetIndices,        // buffer id, format (16/32)
  OpSetShaders,        // vs id, ps id (0 = fixed function)
  OpSetVSConstF,       // start, count, floats...
  OpSetPSConstF,       // start, count, floats...
  OpSetVSConstI, OpSetPSConstI, OpSetVSConstB, OpSetPSConstB,
  OpSetRenderTarget,   // index, kind(0=none,1=backbuffer,2=texture,3=renderbuffer), id, face, level
  OpSetDepthStencil,   // kind(0=none,3=renderbuffer), id
  // Drawing.
  OpClear,             // flags, color, z(float bits), stencil, nrects, rects...
  OpDraw,              // prim, start_vertex, prim_count
  OpDrawIndexed,       // prim, base_vertex, min_index, num_vertices, start_index, prim_count
  OpDrawUP,            // prim, prim_count, stride, nbytes, bytes...          (stream 0 data inline)
  OpDrawIndexedUP,     // prim, min, nverts, prim_count, index_fmt, stride, nvbytes, nibytes, bytes...
  OpStretchRect,       // src kind/id/face/level + rect, dst kind/id/face/level + rect, filter
  OpColorFill,         // kind/id/face/level, rect, color
  OpPresent,           // frame number
  OpSetGammaRamp,      // 3*256 words
  OpReadback,          // kind/id/face/level, w, h, format, dest guest addr, pitch  (sync point)
  OpBackBuffer,        // w, h, vsync, format
  OpResetState,        // (none) Device::Reset: back to default state, nothing bound
};

// Append-only buffer of 32-bit words.
class Stream {
 public:
  std::vector<uint32_t> words;
  void op(Op o, std::initializer_list<uint32_t> args) {
    words.push_back((uint32_t)o);
    words.insert(words.end(), args.begin(), args.end());
  }
  void u32(uint32_t v) { words.push_back(v); }
  void f32(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    words.push_back(v);
  }
  void bytes(const void* p, uint32_t n) {
    size_t at = words.size();
    words.resize(at + (n + 3) / 4);
    memcpy(words.data() + at, p, n);
  }
  void clear() { words.clear(); }
};

// Reader used by the executor.
class Reader {
 public:
  Reader(const uint32_t* p, size_t n) : p_(p), end_(p + n) {}
  bool done() const { return p_ >= end_; }
  uint32_t u32() { return *p_++; }
  int32_t i32() { return (int32_t)*p_++; }
  float f32() {
    float f;
    memcpy(&f, p_++, 4);
    return f;
  }
  const uint8_t* bytes(uint32_t n) {
    const uint8_t* b = (const uint8_t*)p_;
    p_ += (n + 3) / 4;
    return b;
  }

 private:
  const uint32_t* p_;
  const uint32_t* end_;
};

}  // namespace cmd
