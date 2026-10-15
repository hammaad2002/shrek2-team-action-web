// Executor state shared between the command loop (executor.cpp) and the draw path (renderer.cpp).
#pragma once

#include <GLES3/gl3.h>

#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "../d3d9/d3d9.h"

namespace gles {

struct GlFormat {
  GLenum internal = 0, format = 0, type = 0;
  bool compressed = false;
  bool expand = false;  // converted to RGBA8 on the CPU
};

struct Tex {
  GLenum target = GL_TEXTURE_2D;
  GLuint gl = 0;
  uint32_t w = 0, h = 0, levels = 1, fmt = 0;
  bool rt = false;
  GlFormat gfmt;
  // Sampler state last applied to this texture object (GLES has no separate default samplers here).
  uint32_t applied[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
};

struct Rb {
  GLuint gl = 0;
  uint32_t w = 0, h = 0, fmt = 0;
};

struct Buf {
  GLuint gl = 0;
  uint32_t kind = 0, size = 0;
  std::vector<uint8_t> shadow;  // CPU copy (index range scans, client-side conversions)
};

struct ShaderSrc {
  bool pixel = false;
  std::vector<uint32_t> tokens;
};

// A surface reference: kind 0 none, 1 back buffer, 2 texture level, 3 renderbuffer.
struct Target {
  uint32_t kind = 0, id = 0, face = 0, level = 0;
};

struct StreamBinding {
  uint32_t id = 0, offset = 0, stride = 0;
};

struct Exec {
  bool inited = false;
  d3d::State st;
  std::unordered_map<uint32_t, Tex> tex;
  std::unordered_map<uint32_t, Rb> rbs;
  std::unordered_map<uint32_t, Buf> bufs;
  std::unordered_map<uint32_t, ShaderSrc> shaders;
  std::array<uint32_t, 20> textures{};
  std::vector<d3d::VertexElement> layout;
  std::array<StreamBinding, 16> streams{};
  uint32_t index_buffer = 0, index_bits = 16;
  uint32_t vs = 0, ps = 0;
  std::array<Target, 4> rt{};
  Target ds;
  GLuint bb_tex = 0;
  uint32_t bbw = 0, bbh = 0, bb_fmt = 22;
  std::map<uint64_t, GLuint> fbos;
  bool state_dirty = true;
  uint16_t gamma[3 * 256] = {};
  // Statistics.
  uint64_t frames = 0, draws = 0, skipped_draws = 0;
};

extern Exec g;

GlFormat gl_format(uint32_t fmt);
std::vector<uint8_t> to_rgba8(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch);
GLuint fbo_for(const Target& color, const Target& depth);
void target_size(const Target& t, uint32_t* w, uint32_t* h);
void bind_render_target();

void renderer_init();
// One D3D draw call. For *UP draws, `up_*` carry the inline vertex/index data.
void draw(uint32_t prim, uint32_t start, uint32_t count, bool indexed, int32_t base_vertex, uint32_t min_index, uint32_t num_vertices,
          uint32_t up_stride, const uint8_t* up_vdata, uint32_t up_vbytes, const uint8_t* up_idata, uint32_t up_ibytes, uint32_t up_ibits);

}  // namespace gles
