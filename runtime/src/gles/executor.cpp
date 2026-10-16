// GLES3 executor for the D3D9 command stream.
//
// Conventions: every render target (including the back buffer) is an FBO stored "D3D-style": D3D
// row r is GL row y = r. Vertex shaders negate clip-space Y, so D3D viewport/scissor rectangles map
// to GL coordinates unchanged, uploaded textures and rendered textures agree, and only the final
// presentation to the window flips vertically.
#include "executor.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

#include "../core/rt.h"
#include "../d3d9/cmd.h"
#include "../d3d9/d3d9.h"
#include "../platform/platform.h"
#include "renderer.h"

namespace gles {

using namespace d3d;

Exec g;

// ---- Texture formats ---------------------------------------------------------------------------

static bool has_s3tc() {
  static int cached = -1;
  if (cached < 0) {
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    cached = ext && (strstr(ext, "GL_EXT_texture_compression_s3tc") || strstr(ext, "WEBGL_compressed_texture_s3tc")) ? 1 : 0;
    if (!cached) {
      GLint n = 0;
      glGetIntegerv(GL_NUM_EXTENSIONS, &n);
      for (GLint i = 0; i < n; i++) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && (strstr(e, "texture_compression_s3tc") || strstr(e, "compressed_texture_s3tc"))) cached = 1;
      }
    }
    rt::log(rt::Log::Info, "GL: S3TC %s", cached ? "supported" : "not supported (CPU decompression)");
  }
  return cached == 1;
}

GlFormat gl_format(uint32_t fmt) {
  switch (fmt) {
    case FMT_R5G6B5: return {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, false, false};
    case FMT_A1R5G5B5:
    case FMT_X1R5G5B5: return {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, false, false};
    case FMT_A4R4G4B4:
    case FMT_X4R4G4B4: return {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, false, false};
    case FMT_DXT1: if (has_s3tc()) return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, true, false}; break;
    case FMT_DXT2: case FMT_DXT3: if (has_s3tc()) return {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, true, false}; break;
    case FMT_DXT4: case FMT_DXT5: if (has_s3tc()) return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, true, false}; break;
    default: break;
  }
  // Everything else (and DXT without S3TC) is expanded to RGBA8 on the CPU.
  return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false, true};
}

// Decode one DXT colour block (8 bytes) into 16 RGBA pixels.
static void dxt_color(const uint8_t* b, uint8_t* out, bool dxt1) {
  uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
  uint8_t pal[4][4];
  auto expand = [](uint16_t c, uint8_t* p) {
    p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    p[2] = (uint8_t)((c & 31) * 255 / 31);
    p[3] = 255;
  };
  expand(c0, pal[0]);
  expand(c1, pal[1]);
  if (!dxt1 || c0 > c1) {
    for (int k = 0; k < 3; k++) {
      pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  uint32_t idx = (uint32_t)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24);
  for (int i = 0; i < 16; i++) memcpy(out + 4 * i, pal[(idx >> (2 * i)) & 3], 4);
}

// Convert a D3D surface's memory to RGBA8 (row 0 first).
std::vector<uint8_t> to_rgba8(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch) {
  std::vector<uint8_t> out((size_t)w * h * 4);
  FormatInfo fi = format_info(fmt);
  if (fi.block) {
    uint8_t px[64];
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
      for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
        const uint8_t* blk = src + by * pitch + bx * fi.block;
        if (fmt == FMT_DXT1) dxt_color(blk, px, true);
        else {
          dxt_color(blk + 8, px, false);
          if (fmt == FMT_DXT2 || fmt == FMT_DXT3) {
            for (int i = 0; i < 16; i++) px[4 * i + 3] = (uint8_t)(((blk[i / 2] >> (4 * (i & 1))) & 15) * 17);
          } else {
            uint8_t a0 = blk[0], a1 = blk[1], a[8] = {a0, a1};
            if (a0 > a1) for (int k = 1; k < 7; k++) a[k + 1] = (uint8_t)(((7 - k) * a0 + k * a1) / 7);
            else {
              for (int k = 1; k < 5; k++) a[k + 1] = (uint8_t)(((5 - k) * a0 + k * a1) / 5);
              a[6] = 0;
              a[7] = 255;
            }
            uint64_t bits = 0;
            for (int k = 0; k < 6; k++) bits |= (uint64_t)blk[2 + k] << (8 * k);
            for (int i = 0; i < 16; i++) px[4 * i + 3] = a[(bits >> (3 * i)) & 7];
          }
        }
        for (uint32_t y = 0; y < 4; y++)
          for (uint32_t x = 0; x < 4; x++) {
            uint32_t X = bx * 4 + x, Y = by * 4 + y;
            if (X < w && Y < h) memcpy(&out[((size_t)Y * w + X) * 4], &px[(y * 4 + x) * 4], 4);
          }
      }
    return out;
  }
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* s = src + (size_t)y * pitch;
    uint8_t* d = &out[(size_t)y * w * 4];
    for (uint32_t x = 0; x < w; x++, d += 4) {
      switch (fmt) {
        case FMT_A8R8G8B8: d[0] = s[4 * x + 2], d[1] = s[4 * x + 1], d[2] = s[4 * x], d[3] = s[4 * x + 3]; break;
        case FMT_X8R8G8B8: d[0] = s[4 * x + 2], d[1] = s[4 * x + 1], d[2] = s[4 * x], d[3] = 255; break;
        case FMT_A8B8G8R8: memcpy(d, s + 4 * x, 4); break;
        case FMT_X8B8G8R8: memcpy(d, s + 4 * x, 3), d[3] = 255; break;
        case FMT_R8G8B8: d[0] = s[3 * x + 2], d[1] = s[3 * x + 1], d[2] = s[3 * x], d[3] = 255; break;
        case FMT_A8: d[0] = d[1] = d[2] = 0, d[3] = s[x]; break;
        case FMT_L8: d[0] = d[1] = d[2] = s[x], d[3] = 255; break;
        case FMT_A8L8: d[0] = d[1] = d[2] = s[2 * x], d[3] = s[2 * x + 1]; break;
        case FMT_A4L4: d[0] = d[1] = d[2] = (uint8_t)((s[x] & 15) * 17), d[3] = (uint8_t)((s[x] >> 4) * 17); break;
        case FMT_R5G6B5: {
          uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
          d[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31), d[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63), d[2] = (uint8_t)((v & 31) * 255 / 31), d[3] = 255;
          break;
        }
        case FMT_A1R5G5B5:
        case FMT_X1R5G5B5: {
          uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
          d[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31), d[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31), d[2] = (uint8_t)((v & 31) * 255 / 31);
          d[3] = fmt == FMT_X1R5G5B5 || (v & 0x8000) ? 255 : 0;
          break;
        }
        case FMT_A4R4G4B4:
        case FMT_X4R4G4B4: {
          uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
          d[0] = (uint8_t)(((v >> 8) & 15) * 17), d[1] = (uint8_t)(((v >> 4) & 15) * 17), d[2] = (uint8_t)((v & 15) * 17);
          d[3] = fmt == FMT_X4R4G4B4 ? 255 : (uint8_t)((v >> 12) * 17);
          break;
        }
        default: d[0] = 255, d[1] = 0, d[2] = 255, d[3] = 255; break;  // unknown: magenta
      }
    }
  }
  return out;
}

// Repack 16-bit D3D formats into the GL bit layouts (A1R5G5B5 -> RGB5_A1, A4R4G4B4 -> RGBA4).
static std::vector<uint16_t> repack16(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch) {
  std::vector<uint16_t> out((size_t)w * h);
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint16_t v = (uint16_t)(src[y * pitch + 2 * x] | src[y * pitch + 2 * x + 1] << 8);
      uint16_t r;
      if (fmt == FMT_R5G6B5) r = v;
      else if (fmt == FMT_A1R5G5B5) r = (uint16_t)((v << 1) | (v >> 15));
      else if (fmt == FMT_X1R5G5B5) r = (uint16_t)((v << 1) | 1);
      else if (fmt == FMT_A4R4G4B4) r = (uint16_t)((v << 4) | (v >> 12));
      else r = (uint16_t)((v << 4) | 0xf);  // X4R4G4B4
      out[(size_t)y * w + x] = r;
    }
  return out;
}

// ---- Resources -----------------------------------------------------------------------------------

static void create_texture(uint32_t id, uint32_t kind, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, bool rt) {
  Tex t;
  t.target = kind == 1 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
  t.w = w, t.h = h, t.levels = levels, t.fmt = fmt, t.rt = rt;
  glGenTextures(1, &t.gl);
  glBindTexture(t.target, t.gl);
  GlFormat f = rt ? GlFormat{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false, false} : gl_format(fmt);
  t.gfmt = f;
  glTexStorage2D(t.target, (GLsizei)levels, f.internal, (GLsizei)w, (GLsizei)h);
  glTexParameteri(t.target, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
  g.tex[id] = t;
}

static void upload_texture(uint32_t id, uint32_t face, uint32_t level, uint32_t w, uint32_t h, uint32_t fmt, uint32_t pitch, uint32_t rows,
                           const uint8_t* data) {
  auto it = g.tex.find(id);
  if (it == g.tex.end()) return;
  Tex& t = it->second;
  GLenum target = t.target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
  glBindTexture(t.target, t.gl);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  const GlFormat& f = t.gfmt;
  if (f.compressed) {
    glCompressedTexSubImage2D(target, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h, f.internal, (GLsizei)(pitch * rows), data);
  } else if (f.expand) {
    auto rgba = to_rgba8(fmt, data, w, h, pitch);
    if (rt::trace_enabled("uploads")) {
      uint64_t sum = 0;
      for (uint8_t b : rgba) sum += b;
      rt::log(rt::Log::Debug, "upload tex %u level %u %ux%u fmt %08x: mean %.1f, px[center] %02x%02x%02x%02x", id, level, w, h, fmt,
              (double)sum / std::max<size_t>(rgba.size(), 1), rgba[(h / 2 * w + w / 2) * 4], rgba[(h / 2 * w + w / 2) * 4 + 1],
              rgba[(h / 2 * w + w / 2) * 4 + 2], rgba[(h / 2 * w + w / 2) * 4 + 3]);
    }
    glTexSubImage2D(target, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  } else {
    auto px = repack16(fmt, data, w, h, pitch);
    glTexSubImage2D(target, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h, f.format, f.type, px.data());
  }
  (void)rows;
}

static void create_renderbuffer(uint32_t id, uint32_t w, uint32_t h, uint32_t fmt) {
  Rb r;
  r.w = w, r.h = h, r.fmt = fmt;
  glGenRenderbuffers(1, &r.gl);
  glBindRenderbuffer(GL_RENDERBUFFER, r.gl);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
  g.rbs[id] = r;
}

// ---- Render targets ------------------------------------------------------------------------------

static void ensure_backbuffer(uint32_t w, uint32_t h) {
  if (g.bb_tex && g.bbw == w && g.bbh == h) return;
  if (g.bb_tex) glDeleteTextures(1, &g.bb_tex);
  g.bbw = w, g.bbh = h;
  glGenTextures(1, &g.bb_tex);
  glBindTexture(GL_TEXTURE_2D, g.bb_tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
  for (auto& [k, fbo] : g.fbos) glDeleteFramebuffers(1, &fbo);
  g.fbos.clear();
}

// Attach a surface reference to the currently bound draw framebuffer.
static void attach_color(const Target& t, GLenum attachment) {
  if (t.kind == 1) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, g.bb_tex, 0);
  } else if (t.kind == 2) {
    auto it = g.tex.find(t.id);
    if (it == g.tex.end()) return;
    GLenum target = it->second.target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + t.face : GL_TEXTURE_2D;
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, target, it->second.gl, (GLint)t.level);
  }
}

GLuint fbo_for(const Target& color, const Target& depth) {
  uint64_t key = (uint64_t)color.kind << 60 ^ (uint64_t)color.id << 32 ^ (uint64_t)color.face << 28 ^ (uint64_t)color.level << 24 ^
                 (uint64_t)depth.kind << 22 ^ depth.id;
  auto it = g.fbos.find(key);
  if (it != g.fbos.end()) return it->second;
  GLuint fbo;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  attach_color(color, GL_COLOR_ATTACHMENT0);
  if (depth.kind == 3) {
    auto rb = g.rbs.find(depth.id);
    if (rb != g.rbs.end()) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb->second.gl);
  }
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) rt::log(rt::Log::Warn, "GL: incomplete framebuffer %04x (color kind %u id %u)", status, color.kind, color.id);
  g.fbos[key] = fbo;
  return fbo;
}

void target_size(const Target& t, uint32_t* w, uint32_t* h) {
  *w = g.bbw, *h = g.bbh;
  if (t.kind == 2) {
    auto it = g.tex.find(t.id);
    if (it != g.tex.end()) *w = std::max(1u, it->second.w >> t.level), *h = std::max(1u, it->second.h >> t.level);
  } else if (t.kind == 3) {
    auto it = g.rbs.find(t.id);
    if (it != g.rbs.end()) *w = it->second.w, *h = it->second.h;
  }
}

void bind_render_target() {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(g.rt[0], g.ds));
}

// ---- Clears, blits, presentation ---------------------------------------------------------------

static void clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const std::vector<int32_t>& rects) {
  bind_render_target();
  GLbitfield mask = 0;
  if (flags & 1) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f);
    mask |= GL_COLOR_BUFFER_BIT;
  }
  if ((flags & 2) && g.ds.kind) {
    glDepthMask(GL_TRUE);
    glClearDepthf(z);
    mask |= GL_DEPTH_BUFFER_BIT;
  }
  if ((flags & 4) && g.ds.kind) {
    glStencilMask(0xff);
    glClearStencil((GLint)stencil);
    mask |= GL_STENCIL_BUFFER_BIT;
  }
  if (!mask) return;
  // D3D9 clears the viewport (intersected with the scissor rect when scissoring is enabled).
  int32_t x0 = (int32_t)g.st.viewport.x, y0 = (int32_t)g.st.viewport.y;
  int32_t x1 = x0 + (int32_t)g.st.viewport.w, y1 = y0 + (int32_t)g.st.viewport.h;
  if (g.st.rs[174]) {
    x0 = std::max(x0, g.st.scissor.left), y0 = std::max(y0, g.st.scissor.top);
    x1 = std::min(x1, g.st.scissor.right), y1 = std::min(y1, g.st.scissor.bottom);
  }
  glEnable(GL_SCISSOR_TEST);
  auto do_rect = [&](int32_t a0, int32_t b0, int32_t a1, int32_t b1) {
    a0 = std::max(a0, x0), b0 = std::max(b0, y0), a1 = std::min(a1, x1), b1 = std::min(b1, y1);
    if (a1 <= a0 || b1 <= b0) return;
    glScissor(a0, b0, a1 - a0, b1 - b0);
    glClear(mask);
  };
  if (rects.empty()) do_rect(x0, y0, x1, y1);
  else
    for (size_t i = 0; i + 3 < rects.size(); i += 4) do_rect(rects[i], rects[i + 1], rects[i + 2], rects[i + 3]);
  g.state_dirty = true;  // scissor/masks changed behind the draw-state cache
}

// SHREK2_DUMP=n1,n2,... writes those frames (1-based) to $SHREK2_DUMP_DIR (default work/frames)
// as binary PPM, read back from the back buffer just before presentation.
static void maybe_dump_frame() {
  static std::vector<uint64_t> frames;
  static bool parsed = false;
  if (!parsed) {
    parsed = true;
    if (const char* e = getenv("SHREK2_DUMP"))
      for (const char* p = e; *p;) {
        frames.push_back(strtoull(p, (char**)&p, 10));
        while (*p == ',' || *p == ' ') p++;
      }
  }
  if (std::find(frames.begin(), frames.end(), g.frames) == frames.end()) return;
  std::vector<uint8_t> px((size_t)g.bbw * g.bbh * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_for(Target{1, 0, 0, 0}, Target{}));
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, (GLsizei)g.bbw, (GLsizei)g.bbh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  const char* dir = getenv("SHREK2_DUMP_DIR");
  char path[512];
  snprintf(path, sizeof path, "%s/frame_%05llu.ppm", dir ? dir : "work/frames", (unsigned long long)g.frames);
  FILE* f = fopen(path, "wb");
  if (!f) {
    rt::log(rt::Log::Warn, "cannot write %s", path);
    return;
  }
  fprintf(f, "P6\n%u %u\n255\n", g.bbw, g.bbh);
  for (size_t i = 0; i < (size_t)g.bbw * g.bbh; i++) fwrite(&px[i * 4], 1, 3, f);
  fclose(f);
  rt::log(rt::Log::Info, "dumped %s (%llu draws, %llu skipped so far)", path, (unsigned long long)g.draws, (unsigned long long)g.skipped_draws);
}

static void present() {
  g.frames++;
  plat::frame_presented(g.frames);
  rt::note_present();
  maybe_dump_frame();
  int ww, wh;
  plat::get_window_size(&ww, &wh);
  // Look the back buffer's FBO up first: creating it binds GL_FRAMEBUFFER (read and draw).
  GLuint read = fbo_for(Target{1, 0, 0, 0}, Target{});
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, ww, wh);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  // Letterbox to the back buffer's aspect ratio.
  double sa = (double)g.bbw / g.bbh, wa = (double)ww / wh;
  int dw = ww, dh = wh;
  if (wa > sa) dw = (int)std::lround(wh * sa);
  else dh = (int)std::lround(ww / sa);
  int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
  glBlitFramebuffer(0, 0, (GLint)g.bbw, (GLint)g.bbh, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  plat::gl_swap();
  g.state_dirty = true;
}

static void blit(const Target& src, const uint32_t* sr, const Target& dst, const uint32_t* dr, uint32_t filter) {
  GLenum gl_filter = filter == 2 ? GL_LINEAR : GL_NEAREST;
  glDisable(GL_SCISSOR_TEST);
  // Both FBOs are looked up before binding: creating one binds GL_FRAMEBUFFER (read and draw),
  // which used to turn a first blit to a new surface into a blit of that surface onto itself.
  GLuint read = fbo_for(src, Target{}), draw = fbo_for(dst, Target{});
  uint32_t sx0 = sr[0], sy0 = sr[1], sx1 = sr[2], sy1 = sr[3];
  if (src.kind == dst.kind && src.id == dst.id && src.face == dst.face && src.level == dst.level) {
    // StretchRect within one surface: WebGL forbids blitting a framebuffer onto itself, so the
    // source rectangle goes through a scratch texture first.
    uint32_t w = sx1 > sx0 ? sx1 - sx0 : 0, h = sy1 > sy0 ? sy1 - sy0 : 0;
    if (!w || !h) return;
    if (!g.scratch_fbo || g.scratch_w < w || g.scratch_h < h) {
      if (g.scratch_tex) glDeleteTextures(1, &g.scratch_tex);
      if (!g.scratch_fbo) glGenFramebuffers(1, &g.scratch_fbo);
      g.scratch_w = std::max(g.scratch_w, w), g.scratch_h = std::max(g.scratch_h, h);
      glGenTextures(1, &g.scratch_tex);
      glBindTexture(GL_TEXTURE_2D, g.scratch_tex);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)g.scratch_w, (GLsizei)g.scratch_h);
      glBindFramebuffer(GL_FRAMEBUFFER, g.scratch_fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.scratch_tex, 0);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.scratch_fbo);
    glBlitFramebuffer((GLint)sx0, (GLint)sy0, (GLint)sx1, (GLint)sy1, 0, 0, (GLint)w, (GLint)h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    read = g.scratch_fbo;
    sx0 = 0, sy0 = 0, sx1 = w, sy1 = h;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  glBlitFramebuffer((GLint)sx0, (GLint)sy0, (GLint)sx1, (GLint)sy1, (GLint)dr[0], (GLint)dr[1], (GLint)dr[2], (GLint)dr[3], GL_COLOR_BUFFER_BIT,
                    gl_filter);
  g.state_dirty = true;
}

static void readback(const Target& t, uint32_t w, uint32_t h, uint32_t fmt, uint32_t dst, uint32_t pitch) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_for(t, Target{}));
  std::vector<uint8_t> px((size_t)w * h * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  for (uint32_t y = 0; y < h; y++) {
    uint8_t* row = rt::host(dst + y * pitch);
    for (uint32_t x = 0; x < w; x++) {
      const uint8_t* p = &px[((size_t)y * w + x) * 4];
      if (fmt == FMT_R5G6B5) {
        uint16_t v = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
        memcpy(row + 2 * x, &v, 2);
      } else {
        row[4 * x] = p[2], row[4 * x + 1] = p[1], row[4 * x + 2] = p[0], row[4 * x + 3] = fmt == FMT_X8R8G8B8 ? 255 : p[3];
      }
    }
  }
}

// ---- Command loop ------------------------------------------------------------------------------

static void init() {
  if (g.inited) return;
  if (!plat::gl_create_context()) rt::fatal("cannot create an OpenGL ES 3.0 context");
  rt::log(rt::Log::Info, "GL: %s | %s | %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER),
          (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
  init_default_state(g.st);
  renderer_init();
  g.inited = true;
}

void execute(const uint32_t* words, size_t n) {
  init();
  cmd::Reader r(words, n);
  while (!r.done()) {
    uint32_t op = r.u32();
    switch (op) {
      case cmd::OpCreateTexture: {
        uint32_t id = r.u32(), kind = r.u32(), w = r.u32(), h = r.u32(), levels = r.u32(), fmt = r.u32(), isrt = r.u32();
        create_texture(id, kind, w, h, levels, fmt, isrt != 0);
        break;
      }
      case cmd::OpDestroyTexture: {
        uint32_t id = r.u32();
        auto it = g.tex.find(id);
        if (it != g.tex.end()) {
          glDeleteTextures(1, &it->second.gl);
          g.tex.erase(it);
          for (auto f = g.fbos.begin(); f != g.fbos.end();) {
            glDeleteFramebuffers(1, &f->second);
            f = g.fbos.erase(f);
          }
        }
        break;
      }
      case cmd::OpUploadTexture: {
        uint32_t id = r.u32(), face = r.u32(), level = r.u32(), w = r.u32(), h = r.u32(), fmt = r.u32(), pitch = r.u32(), rows = r.u32(),
                 bytes = r.u32();
        upload_texture(id, face, level, w, h, fmt, pitch, rows, r.bytes(bytes));
        break;
      }
      case cmd::OpCreateRenderbuffer: {
        uint32_t id = r.u32(), w = r.u32(), h = r.u32(), fmt = r.u32();
        create_renderbuffer(id, w, h, fmt);
        break;
      }
      case cmd::OpDestroyRenderbuffer: {
        uint32_t id = r.u32();
        auto it = g.rbs.find(id);
        if (it != g.rbs.end()) {
          glDeleteRenderbuffers(1, &it->second.gl);
          g.rbs.erase(it);
        }
        break;
      }
      case cmd::OpCreateBuffer: {
        uint32_t id = r.u32(), kind = r.u32(), size = r.u32();
        Buf b;
        b.kind = kind, b.size = size;
        b.shadow.resize(size);
        glGenBuffers(1, &b.gl);
        GLenum target = kind ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        if (kind) glBindVertexArray(0);
        glBindBuffer(target, b.gl);
        glBufferData(target, size, nullptr, GL_DYNAMIC_DRAW);
        g.bufs[id] = std::move(b);
        break;
      }
      case cmd::OpDestroyBuffer: {
        uint32_t id = r.u32();
        auto it = g.bufs.find(id);
        if (it != g.bufs.end()) {
          glDeleteBuffers(1, &it->second.gl);
          g.bufs.erase(it);
        }
        break;
      }
      case cmd::OpUploadBuffer: {
        uint32_t id = r.u32(), off = r.u32(), size = r.u32();
        const uint8_t* data = r.bytes(size);
        auto it = g.bufs.find(id);
        if (it == g.bufs.end()) break;
        Buf& b = it->second;
        memcpy(b.shadow.data() + off, data, size);
        GLenum target = b.kind ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        if (b.kind) glBindVertexArray(0);
        glBindBuffer(target, b.gl);
        glBufferSubData(target, off, size, data);
        break;
      }
      case cmd::OpCreateShader: {
        uint32_t id = r.u32(), pixel = r.u32(), n = r.u32();
        const uint32_t* t = (const uint32_t*)r.bytes(n * 4);
        g.shaders[id] = ShaderSrc{pixel != 0, std::vector<uint32_t>(t, t + n)};
        break;
      }
      case cmd::OpDestroyShader:
        g.shaders.erase(r.u32());
        break;
      case cmd::OpSetRenderState: {
        uint32_t s = r.u32(), v = r.u32();
        if (s < 256) g.st.rs[s] = v;
        break;
      }
      case cmd::OpSetTextureStageState: {
        uint32_t s = r.u32(), t = r.u32(), v = r.u32();
        if (s < 8 && t < 33) g.st.tss[s][t] = v;
        break;
      }
      case cmd::OpSetSamplerState: {
        uint32_t s = r.u32(), t = r.u32(), v = r.u32();
        if (s < 20 && t < 14) g.st.ss[s][t] = v;
        break;
      }
      case cmd::OpSetTexture: {
        uint32_t s = r.u32(), id = r.u32();
        if (s < 20) g.textures[s] = id;
        break;
      }
      case cmd::OpSetTransform: {
        uint32_t s = r.u32();
        Mat4 m;
        for (float& f : m.m) f = r.f32();
        g.st.transforms[s] = m;
        break;
      }
      case cmd::OpSetViewport: {
        Viewport v;
        v.x = r.u32(), v.y = r.u32(), v.w = r.u32(), v.h = r.u32(), v.min_z = r.f32(), v.max_z = r.f32();
        g.st.viewport = v;
        break;
      }
      case cmd::OpSetScissor:
        g.st.scissor.left = r.i32(), g.st.scissor.top = r.i32(), g.st.scissor.right = r.i32(), g.st.scissor.bottom = r.i32();
        break;
      case cmd::OpSetMaterial:
        memcpy(&g.st.material, r.bytes(68), 68);
        break;
      case cmd::OpSetLight: {
        uint32_t i = r.u32();
        Light l;
        l.type = r.u32();
        memcpy(&l.diffuse, r.bytes(100), 100);
        g.st.lights[i] = l;
        break;
      }
      case cmd::OpLightEnable: {
        uint32_t i = r.u32(), e = r.u32();
        g.st.light_enable[i] = e != 0;
        break;
      }
      case cmd::OpSetClipPlane: {
        uint32_t i = r.u32();
        const float* p = (const float*)r.bytes(16);
        if (i < 6) memcpy(g.st.clip_planes[i].data(), p, 16);
        break;
      }
      case cmd::OpSetVertexLayout: {
        uint32_t n = r.u32();
        g.layout.clear();
        for (uint32_t i = 0; i < n; i++) {
          VertexElement e{};
          e.stream = (uint16_t)r.u32(), e.offset = (uint16_t)r.u32(), e.type = (uint8_t)r.u32(), e.usage = (uint8_t)r.u32(),
          e.usage_index = (uint8_t)r.u32();
          g.layout.push_back(e);
        }
        break;
      }
      case cmd::OpSetStream: {
        uint32_t i = r.u32(), id = r.u32(), off = r.u32(), stride = r.u32();
        if (i < 16) g.streams[i] = {id, off, stride};
        break;
      }
      case cmd::OpSetIndices:
        g.index_buffer = r.u32(), g.index_bits = r.u32();
        break;
      case cmd::OpSetShaders:
        g.vs = r.u32(), g.ps = r.u32();
        break;
      case cmd::OpSetVSConstF: {
        uint32_t start = r.u32(), count = r.u32();
        memcpy(&g.st.vs_f[start * 4], r.bytes(count * 16), count * 16);
        break;
      }
      case cmd::OpSetPSConstF: {
        uint32_t start = r.u32(), count = r.u32();
        memcpy(&g.st.ps_f[start * 4], r.bytes(count * 16), count * 16);
        break;
      }
      case cmd::OpSetVSConstI: case cmd::OpSetPSConstI: {
        uint32_t start = r.u32(), count = r.u32();
        auto& dst = op == cmd::OpSetVSConstI ? g.st.vs_i : g.st.ps_i;
        memcpy(&dst[start * 4], r.bytes(count * 16), count * 16);
        break;
      }
      case cmd::OpSetVSConstB: case cmd::OpSetPSConstB: {
        uint32_t start = r.u32(), count = r.u32();
        auto& dst = op == cmd::OpSetVSConstB ? g.st.vs_b : g.st.ps_b;
        memcpy(&dst[start], r.bytes(count * 4), count * 4);
        break;
      }
      case cmd::OpSetRenderTarget: {
        uint32_t i = r.u32();
        Target t{r.u32(), r.u32(), r.u32(), r.u32()};
        if (i < 4) g.rt[i] = t;
        break;
      }
      case cmd::OpSetDepthStencil:
        g.ds = Target{r.u32(), r.u32(), 0, 0};
        break;
      case cmd::OpClear: {
        uint32_t flags = r.u32(), color = r.u32();
        float z = r.f32();
        uint32_t stencil = r.u32(), nrects = r.u32();
        std::vector<int32_t> rects(nrects * 4);
        for (auto& v : rects) v = r.i32();
        clear(flags, color, z, stencil, rects);
        break;
      }
      case cmd::OpDraw: {
        uint32_t prim = r.u32(), start = r.u32(), count = r.u32();
        draw(prim, start, count, false, 0, 0, 0, 0, nullptr, 0, nullptr, 0, 0);
        break;
      }
      case cmd::OpDrawIndexed: {
        uint32_t prim = r.u32();
        int32_t base = r.i32();
        uint32_t minv = r.u32(), nverts = r.u32(), start = r.u32(), count = r.u32();
        draw(prim, start, count, true, base, minv, nverts, 0, nullptr, 0, nullptr, 0, 0);
        break;
      }
      case cmd::OpDrawUP: {
        uint32_t prim = r.u32(), count = r.u32(), stride = r.u32(), bytes = r.u32();
        const uint8_t* data = r.bytes(bytes);
        draw(prim, 0, count, false, 0, 0, 0, stride, data, bytes, nullptr, 0, 0);
        break;
      }
      case cmd::OpDrawIndexedUP: {
        uint32_t prim = r.u32(), minv = r.u32(), nverts = r.u32(), count = r.u32(), ibits = r.u32(), stride = r.u32(), vbytes = r.u32(),
                 ibytes = r.u32();
        const uint8_t* vdata = r.bytes(vbytes);
        const uint8_t* idata = r.bytes(ibytes);
        draw(prim, 0, count, true, 0, minv, nverts, stride, vdata, vbytes, idata, ibytes, ibits);
        break;
      }
      case cmd::OpStretchRect: {
        Target s{r.u32(), r.u32(), r.u32(), r.u32()};
        uint32_t sr[4] = {r.u32(), r.u32(), r.u32(), r.u32()};
        Target d{r.u32(), r.u32(), r.u32(), r.u32()};
        uint32_t dr[4] = {r.u32(), r.u32(), r.u32(), r.u32()};
        uint32_t filter = r.u32();
        blit(s, sr, d, dr, filter);
        break;
      }
      case cmd::OpColorFill: {
        Target t{r.u32(), r.u32(), r.u32(), r.u32()};
        int32_t rc[4] = {r.i32(), r.i32(), r.i32(), r.i32()};
        uint32_t color = r.u32();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(t, Target{}));
        glEnable(GL_SCISSOR_TEST);
        glScissor(rc[0], rc[1], rc[2] - rc[0], rc[3] - rc[1]);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        g.state_dirty = true;
        break;
      }
      case cmd::OpPresent:
        r.u32();
        present();
        break;
      case cmd::OpSetGammaRamp:
        for (int i = 0; i < 3 * 256; i++) g.gamma[i] = (uint16_t)r.u32();
        break;
      case cmd::OpReadback: {
        Target t{r.u32(), r.u32(), r.u32(), r.u32()};
        uint32_t w = r.u32(), h = r.u32(), fmt = r.u32(), dst = r.u32(), pitch = r.u32();
        readback(t, w, h, fmt, dst, pitch);
        break;
      }
      case cmd::OpResetState:
        init_default_state(g.st);
        g.textures.fill(0);
        g.streams = {};
        g.layout.clear();
        g.vs = g.ps = 0;
        g.index_buffer = 0;
        g.index_bits = 16;
        g.state_dirty = true;
        break;
      case cmd::OpBackBuffer: {
        uint32_t w = r.u32(), h = r.u32(), vsync = r.u32(), fmt = r.u32();
        ensure_backbuffer(w, h);
        g.bb_fmt = fmt;
        // The game asks for immediate presentation; natively we still sync to the display unless
        // SHREK2_VSYNC=0 (the web build is paced by requestAnimationFrame).
        const char* env = getenv("SHREK2_VSYNC");
        bool force = !env || strcmp(env, "0") != 0;
        plat::gl_set_swap_interval(vsync || force ? 1 : 0);
        break;
      }
      default:
        rt::fatal("render stream: unknown op %u", op);
    }
  }
}

}  // namespace gles
