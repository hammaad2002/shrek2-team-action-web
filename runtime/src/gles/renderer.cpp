// Draw path: D3D9 state -> GL state, generated programs, vertex attributes, draw calls.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "../core/rt.h"
#include "programs.h"
#include "renderer.h"

namespace gles {

using namespace d3d;

namespace {

// ---- Matrices (D3D row-major, row-vector convention) ---------------------------------------

Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}

const Mat4& transform(uint32_t s) {
  static const Mat4 ident = Mat4::identity();
  auto it = g.st.transforms.find(s);
  return it == g.st.transforms.end() ? ident : it->second;
}

// Inverse-transpose of the upper 3x3 (normal matrix), as 9 floats row-major (D3D convention).
void normal_matrix(const Mat4& m, float out[9]) {
  float a = m.m[0], b = m.m[1], c = m.m[2], d = m.m[4], e = m.m[5], f = m.m[6], g2 = m.m[8], h = m.m[9], i = m.m[10];
  float A = e * i - f * h, B = -(d * i - f * g2), C = d * h - e * g2;
  float det = a * A + b * B + c * C;
  if (std::fabs(det) < 1e-30f) det = 1.0f;
  float inv = 1.0f / det;
  // inverse(M) = adj(M)/det; normals use (M^-1)^T, which for row vectors is uploaded as M^-1^T.
  float adj[9] = {A, -(b * i - c * h), b * f - c * e, B, a * i - c * g2, -(a * f - c * d), C, -(a * h - b * g2), a * e - b * d};
  // adj is the inverse (times det) in row-major order; transpose it.
  for (int r = 0; r < 3; r++)
    for (int col = 0; col < 3; col++) out[r * 3 + col] = adj[col * 3 + r] * inv;
}

float fbits(uint32_t v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}

void color4(uint32_t c, float* out) {
  out[0] = ((c >> 16) & 255) / 255.0f;
  out[1] = ((c >> 8) & 255) / 255.0f;
  out[2] = (c & 255) / 255.0f;
  out[3] = (c >> 24) / 255.0f;
}

// ---- Programs ---------------------------------------------------------------------------------

struct Program {
  GLuint prog = 0;
  GLint u_vp = -1, u_wv = -1, u_nm = -1, u_proj = -1, u_mat = -1, u_amb = -1, u_light = -1, u_tm = -1, u_fog = -1, u_pt = -1,
        u_ptk = -1;
  GLint u_tf = -1, u_tc = -1, u_fogc = -1, u_fogp = -1, u_aref = -1, u_c = -1;
  GLint samplers[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  uint32_t nconst = 0;
  bool ok = false;
};

std::unordered_map<std::string, Program> g_programs;

GLuint compile(GLenum type, const std::string& src) {
  GLuint s = glCreateShader(type);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    rt::log(rt::Log::Error, "GL: shader compile failed:\n%s\n--- source ---\n%s", log, src.c_str());
    glDeleteShader(s);
    return 0;
  }
  return s;
}

Program build_program(const ProgKey& k) {
  Program p;
  std::string vs;
  if (k.vs_kind == 2) {
    auto it = g.shaders.find(k.vs_id);
    if (it == g.shaders.end()) return p;
    VsInfo info;
    std::string err;
    vs = sm_vertex_glsl(it->second.tokens, k, &info, &err);
    if (vs.empty()) {
      rt::log(rt::Log::Warn, "GL: cannot translate vertex shader %u: %s", k.vs_id, err.c_str());
      return p;
    }
    p.nconst = info.nconst;
  } else {
    vs = ff_vertex_glsl(k);
  }
  std::string fs = ff_fragment_glsl(k);
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return p;
  p.prog = glCreateProgram();
  glAttachShader(p.prog, v);
  glAttachShader(p.prog, f);
  glLinkProgram(p.prog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(p.prog, sizeof log, nullptr, log);
    rt::log(rt::Log::Error, "GL: program link failed: %s\n--- vs ---\n%s\n--- fs ---\n%s", log, vs.c_str(), fs.c_str());
    glDeleteProgram(p.prog);
    p.prog = 0;
    return p;
  }
  auto loc = [&](const char* n) { return glGetUniformLocation(p.prog, n); };
  p.u_vp = loc("u_vp");
  p.u_wv = loc("u_wv");
  p.u_nm = loc("u_nm");
  p.u_proj = loc("u_proj");
  p.u_mat = loc("u_mat");
  p.u_amb = loc("u_amb");
  p.u_light = loc("u_light");
  p.u_tm = loc("u_tm");
  p.u_fog = loc("u_fog");
  p.u_pt = loc("u_pt");
  p.u_ptk = loc("u_ptk");
  p.u_tf = loc("u_tf");
  p.u_tc = loc("u_tc");
  p.u_fogc = loc("u_fogc");
  p.u_fogp = loc("u_fogp");
  p.u_aref = loc("u_aref");
  p.u_c = loc("c");
  glUseProgram(p.prog);
  for (int i = 0; i < 8; i++) {
    char n[8];
    snprintf(n, sizeof n, "s%d", i);
    p.samplers[i] = loc(n);
    if (p.samplers[i] >= 0) glUniform1i(p.samplers[i], i);
  }
  p.ok = true;
  if (rt::trace_enabled("shaders")) rt::log(rt::Log::Debug, "GL: new program\n--- vs ---\n%s--- fs ---\n%s", vs.c_str(), fs.c_str());
  return p;
}

// ---- Streaming buffers for *UP draws -----------------------------------------------------------

GLuint g_vao = 0, g_up_vbo = 0, g_up_ibo = 0;
uint32_t g_enabled_attribs = 0;

// ---- Samplers ----------------------------------------------------------------------------------

std::unordered_map<uint64_t, GLuint> g_samplers;

GLenum gl_address(uint32_t a) {
  switch (a) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: case 4: return GL_CLAMP_TO_EDGE;  // BORDER is not available in GLES 3.0
    case 5: return GL_MIRRORED_REPEAT;
    default: return GL_REPEAT;
  }
}

GLuint sampler_for(uint32_t stage, bool mipmapped) {
  const auto& ss = g.st.ss[stage];
  uint32_t mag = ss[5], min = ss[6], mip = mipmapped ? ss[7] : 0;
  uint64_t key = (uint64_t)ss[1] | (uint64_t)ss[2] << 4 | (uint64_t)ss[3] << 8 | (uint64_t)mag << 12 | (uint64_t)min << 16 |
                 (uint64_t)mip << 20 | (uint64_t)std::min<uint32_t>(ss[9], 15) << 24 | (uint64_t)std::min<uint32_t>(ss[10], 16) << 28;
  auto it = g_samplers.find(key);
  if (it != g_samplers.end()) return it->second;
  GLuint s;
  glGenSamplers(1, &s);
  glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)gl_address(ss[1]));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)gl_address(ss[2]));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_R, (GLint)gl_address(ss[3]));
  bool mag_lin = mag >= 2, min_lin = min >= 2;
  glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
  GLenum mf;
  if (mip == 0) mf = min_lin ? GL_LINEAR : GL_NEAREST;
  else if (mip == 1) mf = min_lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
  else mf = min_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
  glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)mf);
  glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)ss[9]);
  if (min == 3 || mag == 3) glSamplerParameterf(s, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, (float)std::max<uint32_t>(ss[10], 1));
  g_samplers[key] = s;
  return s;
}

// ---- GL state cache ---------------------------------------------------------------------------

struct GlState {
  int depth_test = -1, depth_mask = -1, depth_func = -1, cull = -1, cull_face = -1, blend = -1, scissor = -1, stencil = -1,
      poly_offset = -1;
  GLenum bsrc = 0, bdst = 0, bsrca = 0, bdsta = 0, bop = 0, bopa = 0;
  uint32_t color_mask = ~0u, blend_color = 0x12345678;
  int vp[4] = {-1, -1, -1, -1}, sc[4] = {-1, -1, -1, -1};
  float dr[2] = {-1, -1};
  float po[2] = {0, 0};
  GLuint program = 0;
} gs;

void set_cap(GLenum cap, int on, int& cached) {
  if (cached == on) return;
  cached = on;
  if (on) glEnable(cap);
  else glDisable(cap);
}

GLenum gl_cmp(uint32_t f) {
  static const GLenum m[9] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
  return f < 9 ? m[f] : GL_ALWAYS;
}

GLenum gl_blend(uint32_t b, bool rt_alpha) {
  switch (b) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return rt_alpha ? GL_DST_ALPHA : GL_ONE;
    case 8: return rt_alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    case 14: return GL_CONSTANT_COLOR;
    case 15: return GL_ONE_MINUS_CONSTANT_COLOR;
    default: return GL_ONE;
  }
}

GLenum gl_blend_op(uint32_t o) {
  switch (o) {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
  }
}

GLenum gl_stencil_op(uint32_t o) {
  static const GLenum m[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
  return o < 9 ? m[o] : GL_KEEP;
}

bool target_has_alpha() {
  const Target& t = g.rt[0];
  uint32_t fmt = g.bb_fmt;
  if (t.kind == 2) {
    auto it = g.tex.find(t.id);
    if (it != g.tex.end()) fmt = it->second.fmt;
  }
  return !(fmt == FMT_X8R8G8B8 || fmt == FMT_R5G6B5 || fmt == FMT_X1R5G5B5 || fmt == FMT_X4R4G4B4 || fmt == FMT_R8G8B8);
}

void apply_state(bool rhw) {
  const auto& rs = g.st.rs;
  if (g.state_dirty) {
    // Clears and blits changed state behind the cache.
    gs = GlState{};
    g.state_dirty = false;
  }
  bind_render_target();
  // Viewport and depth range.
  const Viewport& v = g.st.viewport;
  int vp[4] = {(int)v.x, (int)v.y, (int)v.w, (int)v.h};
  if (memcmp(vp, gs.vp, sizeof vp)) {
    memcpy(gs.vp, vp, sizeof vp);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
  }
  float dr[2] = {rhw ? 0.0f : v.min_z, rhw ? 1.0f : v.max_z};
  if (dr[0] != gs.dr[0] || dr[1] != gs.dr[1]) {
    gs.dr[0] = dr[0], gs.dr[1] = dr[1];
    glDepthRangef(dr[0], dr[1]);
  }
  // Scissor.
  set_cap(GL_SCISSOR_TEST, rs[174] ? 1 : 0, gs.scissor);
  if (rs[174]) {
    const auto& s = g.st.scissor;
    int sc[4] = {s.left, s.top, std::max(0, s.right - s.left), std::max(0, s.bottom - s.top)};
    if (memcmp(sc, gs.sc, sizeof sc)) {
      memcpy(gs.sc, sc, sizeof sc);
      glScissor(sc[0], sc[1], sc[2], sc[3]);
    }
  }
  // Depth.
  bool has_depth = g.ds.kind != 0;
  set_cap(GL_DEPTH_TEST, has_depth && rs[7] ? 1 : 0, gs.depth_test);
  int dm = has_depth && rs[14] ? 1 : 0;
  if (dm != gs.depth_mask) {
    gs.depth_mask = dm;
    glDepthMask(dm ? GL_TRUE : GL_FALSE);
  }
  int df = (int)gl_cmp(rs[23]);
  if (df != gs.depth_func) {
    gs.depth_func = df;
    glDepthFunc((GLenum)df);
  }
  // Depth bias.
  float bias = fbits(rs[195]), slope = fbits(rs[175]);
  set_cap(GL_POLYGON_OFFSET_FILL, (bias != 0 || slope != 0) ? 1 : 0, gs.poly_offset);
  if ((bias != 0 || slope != 0) && (bias != gs.po[0] || slope != gs.po[1])) {
    gs.po[0] = bias, gs.po[1] = slope;
    glPolygonOffset(slope, bias * 16777216.0f);
  }
  // Culling (render targets are stored D3D-style, so D3D's clockwise front faces are GL's CCW).
  uint32_t cull = rs[22];
  set_cap(GL_CULL_FACE, cull == 2 || cull == 3 ? 1 : 0, gs.cull);
  if (cull == 2 || cull == 3) {
    int cf = cull == 3 ? GL_BACK : GL_FRONT;
    if (cf != gs.cull_face) {
      gs.cull_face = cf;
      glCullFace((GLenum)cf);
    }
  }
  // Blending.
  set_cap(GL_BLEND, rs[27] ? 1 : 0, gs.blend);
  if (rs[27]) {
    bool rta = target_has_alpha();
    uint32_t sb = rs[19], db = rs[20];
    GLenum src = gl_blend(sb, rta), dst = gl_blend(db, rta);
    if (sb == 12) src = GL_SRC_ALPHA, dst = GL_ONE_MINUS_SRC_ALPHA;
    if (sb == 13) src = GL_ONE_MINUS_SRC_ALPHA, dst = GL_SRC_ALPHA;
    GLenum srca = src, dsta = dst, op = gl_blend_op(rs[171]), opa = op;
    if (rs[206]) {
      srca = gl_blend(rs[207], rta), dsta = gl_blend(rs[208], rta), opa = gl_blend_op(rs[209]);
    }
    if (src != gs.bsrc || dst != gs.bdst || srca != gs.bsrca || dsta != gs.bdsta) {
      gs.bsrc = src, gs.bdst = dst, gs.bsrca = srca, gs.bdsta = dsta;
      glBlendFuncSeparate(src, dst, srca, dsta);
    }
    if (op != gs.bop || opa != gs.bopa) {
      gs.bop = op, gs.bopa = opa;
      glBlendEquationSeparate(op, opa);
    }
    if (rs[193] != gs.blend_color) {
      gs.blend_color = rs[193];
      float c[4];
      color4(rs[193], c);
      glBlendColor(c[0], c[1], c[2], c[3]);
    }
  }
  uint32_t cm = rs[168] & 0xf;
  if (cm != gs.color_mask) {
    gs.color_mask = cm;
    glColorMask(cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, (cm >> 3) & 1);
  }
  // Stencil (two-sided: D3D's CCW faces are GL back faces).
  bool st = has_depth && rs[52];
  set_cap(GL_STENCIL_TEST, st ? 1 : 0, gs.stencil);
  if (st) {
    GLint ref = (GLint)(rs[57] & 0xff);
    GLuint mask = rs[58] & 0xff;
    glStencilMask(rs[59] & 0xff);
    glStencilFuncSeparate(GL_FRONT, gl_cmp(rs[56]), ref, mask);
    glStencilOpSeparate(GL_FRONT, gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
    if (rs[185]) {
      glStencilFuncSeparate(GL_BACK, gl_cmp(rs[189]), ref, mask);
      glStencilOpSeparate(GL_BACK, gl_stencil_op(rs[186]), gl_stencil_op(rs[187]), gl_stencil_op(rs[188]));
    } else {
      glStencilFuncSeparate(GL_BACK, gl_cmp(rs[56]), ref, mask);
      glStencilOpSeparate(GL_BACK, gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
    }
  }
}

// ---- Vertex input ------------------------------------------------------------------------------

struct AttrFormat {
  GLint size;
  GLenum type;
  GLboolean norm;
  uint32_t bytes;
};

AttrFormat attr_format(uint8_t t) {
  switch (t) {
    case 0: return {1, GL_FLOAT, GL_FALSE, 4};
    case 1: return {2, GL_FLOAT, GL_FALSE, 8};
    case 2: return {3, GL_FLOAT, GL_FALSE, 12};
    case 3: return {4, GL_FLOAT, GL_FALSE, 16};
    case 4: return {4, GL_UNSIGNED_BYTE, GL_TRUE, 4};
    case 5: return {4, GL_UNSIGNED_BYTE, GL_FALSE, 4};
    case 6: return {2, GL_SHORT, GL_FALSE, 4};
    case 7: return {4, GL_SHORT, GL_FALSE, 8};
    case 8: return {4, GL_UNSIGNED_BYTE, GL_TRUE, 4};
    case 9: return {2, GL_SHORT, GL_TRUE, 4};
    case 10: return {4, GL_SHORT, GL_TRUE, 8};
    case 11: return {2, GL_UNSIGNED_SHORT, GL_TRUE, 4};
    case 12: return {4, GL_UNSIGNED_SHORT, GL_TRUE, 8};
    case 13: return {4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_FALSE, 4};
    case 14: return {4, GL_INT_2_10_10_10_REV, GL_TRUE, 4};
    case 15: return {2, GL_HALF_FLOAT, GL_FALSE, 4};
    case 16: return {4, GL_HALF_FLOAT, GL_FALSE, 8};
    default: return {4, GL_FLOAT, GL_FALSE, 16};
  }
}

uint32_t element_dims(uint8_t t) {
  switch (t) {
    case 0: return 1;
    case 1: case 6: case 9: case 11: case 15: return 2;
    case 2: return 3;
    default: return 4;
  }
}

// Bind vertex attributes for the current layout. `up` overrides stream 0 with the UP buffer.
bool setup_attributes(int32_t base_vertex, bool up, uint32_t up_stride) {
  uint32_t enabled = 0;
  for (const VertexElement& e : g.layout) {
    int loc = attr_location(e.usage, e.usage_index);
    if (loc < 0 || (enabled >> loc) & 1) continue;
    GLuint buf;
    uint32_t stride, offset;
    if (up && e.stream == 0) {
      buf = g_up_vbo;
      stride = up_stride;
      offset = 0;
    } else {
      const StreamBinding& sb = g.streams[e.stream & 15];
      auto it = g.bufs.find(sb.id);
      if (it == g.bufs.end()) continue;
      buf = it->second.gl;
      stride = sb.stride;
      offset = sb.offset;
    }
    int64_t off = (int64_t)offset + e.offset + (int64_t)base_vertex * stride;
    if (off < 0) return false;
    AttrFormat f = attr_format(e.type);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glVertexAttribPointer((GLuint)loc, f.size, f.type, f.norm, (GLsizei)stride, (const void*)(uintptr_t)off);
    enabled |= 1u << loc;
  }
  uint32_t changed = enabled ^ g_enabled_attribs;
  for (int i = 0; i < 16; i++)
    if ((changed >> i) & 1) {
      if ((enabled >> i) & 1) glEnableVertexAttribArray((GLuint)i);
      else glDisableVertexAttribArray((GLuint)i);
    }
  g_enabled_attribs = enabled;
  return true;
}

// ---- Program key -------------------------------------------------------------------------------

void build_key(ProgKey& k, uint32_t prim) {
  memset(&k, 0, sizeof k);
  const auto& rs = g.st.rs;
  bool rhw = false;
  uint32_t tex_dims[8] = {};
  for (const VertexElement& e : g.layout) {
    int loc = attr_location(e.usage, e.usage_index);
    if (loc < 0) continue;
    k.attr_mask |= (uint16_t)(1u << loc);
    if (e.type == 4) k.bgra_mask |= (uint16_t)(1u << loc);
    if (loc == kLocPosition) {
      if (e.usage == 9) rhw = true;
      else if (element_dims(e.type) == 4) k.pos4 = 1;
    }
    if (loc >= kLocTex0 && loc < kLocTex0 + 8) tex_dims[loc - kLocTex0] = element_dims(e.type);
  }
  bool shader = g.vs != 0 && g.shaders.count(g.vs);
  k.vs_kind = shader ? 2 : rhw ? 1 : 0;
  k.vs_id = shader ? g.vs : 0;
  k.points = prim == 1;
  if (!shader && !rhw) {
    bool has_normal = (k.attr_mask >> kLocNormal) & 1;
    k.lighting = rs[137] ? 1 : 0;
    if (k.lighting) {
      for (auto& [i, en] : g.st.light_enable) {
        if (!en || k.nlights >= 8) continue;
        auto it = g.st.lights.find(i);
        if (it == g.st.lights.end()) continue;
        k.light_type[k.nlights++] = (uint8_t)std::clamp<uint32_t>(it->second.type, 1, 3);
      }
      k.specular = rs[29] ? 1 : 0;
      k.local_viewer = rs[142] ? 1 : 0;
      k.normalize = rs[143] ? 1 : 0;
      bool cv = rs[141] != 0;
      auto src = [&](uint32_t s) -> uint8_t {
        if (!cv) return 0;
        if (s == 1 && ((k.attr_mask >> kLocColor0) & 1)) return 1;
        if (s == 2 && ((k.attr_mask >> kLocColor1) & 1)) return 2;
        return 0;
      };
      k.dsrc = src(rs[145]), k.ssrc = src(rs[146]), k.asrc = src(rs[147]), k.esrc = src(rs[148]);
    }
    (void)has_normal;
    uint32_t vb = rs[151];
    if (vb >= 1 && vb <= 3) k.blend = (uint8_t)(vb + 1);
    k.point_scale = k.points && rs[157] ? 1 : 0;
  }
  // Fog.
  if (rs[28]) {
    uint32_t table = rs[35], vmode = rs[140];
    if (table) {
      k.vfog = 1;
      k.pfog = (uint8_t)(1 + std::min<uint32_t>(table, 3));
      const Mat4& p = transform(3);
      k.wfog = p.m[11] != 0.0f ? 1 : 0;
    } else if (vmode && !rhw && !shader) {
      k.vfog = (uint8_t)(1 + std::min<uint32_t>(vmode, 3));
      k.range_fog = rs[48] ? 1 : 0;
      k.pfog = 1;
    } else {
      k.vfog = 5;
      k.pfog = 1;
    }
  }
  // Texture stages.
  int n = 0;
  for (; n < 8; n++) {
    const auto& ts = g.st.tss[n];
    if (ts[1] == 1) break;  // COLOROP DISABLE ends the cascade
    k.cop[n] = (uint8_t)ts[1];
    k.ca[n][0] = (uint8_t)ts[26], k.ca[n][1] = (uint8_t)ts[2], k.ca[n][2] = (uint8_t)ts[3];
    k.aop[n] = (uint8_t)ts[4];
    k.aa[n][0] = (uint8_t)ts[27], k.aa[n][1] = (uint8_t)ts[5], k.aa[n][2] = (uint8_t)ts[6];
    k.res[n] = (uint8_t)ts[28];
    auto it = g.textures[n] ? g.tex.find(g.textures[n]) : g.tex.end();
    k.tex_kind[n] = it == g.tex.end() ? 0 : it->second.target == GL_TEXTURE_CUBE_MAP ? 2 : 1;
    uint32_t tci = ts[11], ttff = ts[24];
    uint32_t count = ttff & 0xff;
    if (shader) {
      k.tc_index[n] = (uint8_t)(tci & 7);
    } else {
      k.tc_index[n] = (uint8_t)n;
      uint32_t gen = tci & 0xffff0000u;
      k.tc_src[n] = gen ? (uint8_t)(std::min<uint32_t>(gen >> 16, 4) << 4) : (uint8_t)(tci & 7);
      k.tc_dims[n] = gen ? 3 : (uint8_t)tex_dims[tci & 7];
      if (!rhw && count) k.tc_count[n] = (uint8_t)count;
    }
    if (ttff & 0x100) {
      uint32_t c = count ? count : (shader ? 4 : k.tc_dims[n]);
      if (c == 3 || c == 4) k.tex_proj[n] = (uint8_t)c;
    }
  }
  k.nstages = (uint8_t)n;
  k.ntc = shader ? 0 : (uint8_t)n;
  k.spec_add = rs[29] ? 1 : 0;
  k.alpha_func = rs[15] && rs[25] != 8 ? (uint8_t)rs[25] : 0;
  k.point_sprite = k.points && rs[156] ? 1 : 0;
}

// ---- Uniforms ------------------------------------------------------------------------------------

void upload_uniforms(const Program& p, const ProgKey& k) {
  const auto& rs = g.st.rs;
  const Viewport& v = g.st.viewport;
  if (p.u_vp >= 0) glUniform4f(p.u_vp, (float)v.x, (float)v.y, (float)std::max(v.w, 1u), (float)std::max(v.h, 1u));
  if (k.vs_kind == 2) {
    if (p.u_c >= 0 && p.nconst) glUniform4fv(p.u_c, (GLsizei)p.nconst, g.st.vs_f.data());
  } else if (k.vs_kind == 0) {
    const Mat4& view = transform(2);
    float wv[4 * 16], nm[4 * 9];
    int nmat = std::max<int>(1, k.blend);
    for (int i = 0; i < nmat; i++) {
      Mat4 m = mul(transform(256 + (uint32_t)i), view);
      memcpy(&wv[16 * i], m.m, 64);
      normal_matrix(m, &nm[9 * i]);
    }
    if (p.u_wv >= 0) glUniformMatrix4fv(p.u_wv, nmat, GL_FALSE, wv);
    if (p.u_nm >= 0) glUniformMatrix3fv(p.u_nm, nmat, GL_FALSE, nm);
    if (p.u_proj >= 0) glUniformMatrix4fv(p.u_proj, 1, GL_FALSE, transform(3).m);
    if (p.u_mat >= 0) {
      const Material& m = g.st.material;
      float mat[20];
      memcpy(mat, m.diffuse, 16);
      memcpy(mat + 4, m.ambient, 16);
      memcpy(mat + 8, m.specular, 16);
      memcpy(mat + 12, m.emissive, 16);
      mat[16] = m.power, mat[17] = mat[18] = mat[19] = 0;
      glUniform4fv(p.u_mat, 5, mat);
    }
    if (p.u_amb >= 0) {
      float a[4];
      color4(rs[139], a);
      glUniform4fv(p.u_amb, 1, a);
    }
    if (p.u_light >= 0 && k.nlights) {
      float L[56 * 4] = {};
      int n = 0;
      for (auto& [i, en] : g.st.light_enable) {
        if (!en || n >= k.nlights) continue;
        auto it = g.st.lights.find(i);
        if (it == g.st.lights.end()) continue;
        const Light& l = it->second;
        float* o = &L[n * 28];
        memcpy(o, l.diffuse, 16);
        memcpy(o + 4, l.specular, 16);
        memcpy(o + 8, l.ambient, 16);
        // Position and direction in view space.
        const float* V = view.m;
        for (int c = 0; c < 3; c++) {
          o[12 + c] = l.position[0] * V[c] + l.position[1] * V[4 + c] + l.position[2] * V[8 + c] + V[12 + c];
          o[16 + c] = l.direction[0] * V[c] + l.direction[1] * V[4 + c] + l.direction[2] * V[8 + c];
        }
        float dl = std::sqrt(o[16] * o[16] + o[17] * o[17] + o[18] * o[18]);
        if (dl > 0) o[16] /= dl, o[17] /= dl, o[18] /= dl;
        o[15] = l.range;
        o[19] = l.falloff;
        o[20] = l.att0, o[21] = l.att1, o[22] = l.att2, o[23] = std::cos(l.theta * 0.5f);
        o[24] = std::cos(l.phi * 0.5f);
        n++;
      }
      glUniform4fv(p.u_light, n * 7, L);
    }
    if (p.u_tm >= 0) {
      float tm[8 * 16];
      for (int i = 0; i < 8; i++) memcpy(&tm[16 * i], transform(16 + (uint32_t)i).m, 64);
      glUniformMatrix4fv(p.u_tm, 8, GL_FALSE, tm);
    }
  }
  if (p.u_fog >= 0 || p.u_fogp >= 0) {
    float f[4] = {fbits(rs[36]), fbits(rs[37]), fbits(rs[38]), 0};
    if (p.u_fog >= 0) glUniform4fv(p.u_fog, 1, f);
    if (p.u_fogp >= 0) glUniform4fv(p.u_fogp, 1, f);
  }
  if (p.u_fogc >= 0) {
    float c[4];
    color4(rs[34], c);
    glUniform4fv(p.u_fogc, 1, c);
  }
  if (p.u_pt >= 0) glUniform4f(p.u_pt, fbits(rs[154]), fbits(rs[155]), fbits(rs[166]), 0);
  if (p.u_ptk >= 0) glUniform4f(p.u_ptk, fbits(rs[158]), fbits(rs[159]), fbits(rs[160]), 0);
  if (p.u_tf >= 0) {
    float c[4];
    color4(rs[60], c);
    glUniform4fv(p.u_tf, 1, c);
  }
  if (p.u_tc >= 0) {
    float c[32];
    for (int i = 0; i < 8; i++) color4(g.st.tss[i][32], &c[4 * i]);
    glUniform4fv(p.u_tc, 8, c);
  }
  if (p.u_aref >= 0) glUniform1f(p.u_aref, (float)(rs[24] & 0xff));
}

void bind_textures(const ProgKey& k) {
  for (int i = 0; i < k.nstages; i++) {
    if (!k.tex_kind[i]) continue;
    auto it = g.tex.find(g.textures[i]);
    if (it == g.tex.end()) continue;
    glActiveTexture(GL_TEXTURE0 + (GLenum)i);
    glBindTexture(it->second.target, it->second.gl);
    glBindSampler((GLuint)i, sampler_for((uint32_t)i, it->second.levels > 1));
  }
}

GLenum gl_prim(uint32_t p) {
  switch (p) {
    case 1: return GL_POINTS;
    case 2: return GL_LINES;
    case 3: return GL_LINE_STRIP;
    case 5: return GL_TRIANGLE_STRIP;
    case 6: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
  }
}

uint32_t prim_vertices(uint32_t prim, uint32_t n) {
  switch (prim) {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5: case 6: return n + 2;
    default: return 0;
  }
}

}  // namespace

void renderer_init() {
  glGenVertexArrays(1, &g_vao);
  glBindVertexArray(g_vao);
  glGenBuffers(1, &g_up_vbo);
  glGenBuffers(1, &g_up_ibo);
  for (int i = 0; i < 16; i++) glVertexAttrib4f((GLuint)i, 0, 0, 0, 1);
  glFrontFace(GL_CCW);
  GLint n = 0;
  glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &n);
  rt::log(rt::Log::Info, "GL: %d vertex uniform vectors", n);
}

void draw(uint32_t prim, uint32_t start, uint32_t count, bool indexed, int32_t base_vertex, uint32_t min_index, uint32_t num_vertices,
          uint32_t up_stride, const uint8_t* up_vdata, uint32_t up_vbytes, const uint8_t* up_idata, uint32_t up_ibytes, uint32_t up_ibits) {
  g.draws++;
  uint32_t nverts = prim_vertices(prim, count);
  if (!nverts || g.layout.empty()) {
    g.skipped_draws++;
    return;
  }
  ProgKey key;
  build_key(key, prim);
  std::string ks((const char*)&key, sizeof key);
  auto it = g_programs.find(ks);
  if (it == g_programs.end()) it = g_programs.emplace(ks, build_program(key)).first;
  const Program& p = it->second;
  if (!p.ok) {
    g.skipped_draws++;
    return;
  }
  apply_state(key.vs_kind == 1);
  if (gs.program != p.prog) {
    gs.program = p.prog;
    glUseProgram(p.prog);
  }
  upload_uniforms(p, key);
  bind_textures(key);
  glBindVertexArray(g_vao);
  bool up = up_vdata != nullptr;
  if (rt::trace_enabled("draws")) {
    rt::log(rt::Log::Debug, "draw prim %u count %u start %u indexed %d base %d up %d stride %u layout %zu tex0 %u", prim, count, start, indexed,
            base_vertex, up, up_stride, g.layout.size(), g.textures[0]);
    if (up && up_vbytes >= 32) {
      const float* f = (const float*)up_vdata;
      const uint32_t* u = (const uint32_t*)up_vdata;
      rt::log(rt::Log::Debug, "  v0: %g %g %g %g | %08x | %g %g", f[0], f[1], f[2], f[3], u[4], f[5], f[6]);
    }
    GLenum e = glGetError();
    if (e) rt::log(rt::Log::Warn, "GL error %04x before draw", e);
  }
  if (up) {
    glBindBuffer(GL_ARRAY_BUFFER, g_up_vbo);
    glBufferData(GL_ARRAY_BUFFER, up_vbytes, up_vdata, GL_STREAM_DRAW);
  }
  GLenum mode = gl_prim(prim);
  if (!indexed) {
    if (!setup_attributes(0, up, up_stride)) return;
    glDrawArrays(mode, (GLint)start, (GLsizei)nverts);
    return;
  }
  GLenum itype;
  uint32_t isize;
  if (up) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_up_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, up_ibytes, up_idata, GL_STREAM_DRAW);
    isize = up_ibits / 8;
    start = 0;
  } else {
    auto ib = g.bufs.find(g.index_buffer);
    if (ib == g.bufs.end()) {
      g.skipped_draws++;
      return;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->second.gl);
    isize = g.index_bits / 8;
  }
  itype = isize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
  if (!setup_attributes(base_vertex, up, up_stride)) {
    g.skipped_draws++;
    return;
  }
  (void)min_index;
  (void)num_vertices;
  glDrawElements(mode, (GLsizei)nverts, itype, (const void*)(uintptr_t)(start * isize));
}

}  // namespace gles
