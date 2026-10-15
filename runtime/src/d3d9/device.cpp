// IDirect3DDevice9: state tracking (for Get* calls and state blocks) plus command recording.
#include <cmath>

#include "../core/objects.h"
#include "recorder.h"

using namespace rt;

namespace d3d {

void fill_caps(uint32_t p);

namespace {

Device* D(ComObject* o) { return static_cast<Device*>(o); }
uint32_t ok(Cpu*, ComObject*) { return S_OK; }
float fl(uint32_t v) { return bits2f32(v); }

template <typename T>
T* as(uint32_t guest) {
  return guest ? com_as<T>(guest) : nullptr;
}

void addref(ComObject* o) { add_ref(o); }

// ---- State block recording ---------------------------------------------------------------------
// While a block is being recorded, Set* calls are captured instead of applied (D3D9 semantics).
struct RecOp {
  uint32_t kind, a, b;
  std::vector<uint32_t> data;
};
std::map<StateBlock*, std::vector<RecOp>> g_recorded;

enum RecKind : uint32_t { RK_RS, RK_TSS, RK_SS, RK_TEX, RK_XFORM, RK_VP, RK_MAT, RK_LIGHT, RK_LIGHTEN, RK_FVF, RK_DECL, RK_VS, RK_PS, RK_STREAM, RK_IND, RK_VSC, RK_PSC, RK_SCISSOR, RK_CLIP };

bool recording(Device* d, RecOp op) {
  if (!d->recording) return false;
  g_recorded[d->recording].push_back(std::move(op));
  return true;
}

// ---- Primitive state setters (also used when applying state blocks) --------------------------

void set_rs(Device* d, uint32_t s, uint32_t v) {
  if (s >= 256) return;
  d->st.rs[s] = v;
  rec().op(cmd::OpSetRenderState, {s, v});
}

void set_tss(Device* d, uint32_t stage, uint32_t t, uint32_t v) {
  if (stage >= 8 || t >= 33) return;
  d->st.tss[stage][t] = v;
  rec().op(cmd::OpSetTextureStageState, {stage, t, v});
}

uint32_t sampler_index(uint32_t s) { return s >= 257 && s <= 260 ? 16 + (s - 257) : s; }

void set_ss(Device* d, uint32_t sampler, uint32_t t, uint32_t v) {
  sampler = sampler_index(sampler);
  if (sampler >= 20 || t >= 14) return;
  d->st.ss[sampler][t] = v;
  rec().op(cmd::OpSetSamplerState, {sampler, t, v});
}

uint32_t texture_id(BaseTexture* t) { return t ? t->gl_tex : 0; }

void set_texture(Device* d, uint32_t stage, BaseTexture* t) {
  stage = sampler_index(stage);
  if (stage >= 20) return;
  if (d->st.textures[stage] == t) return;
  add_ref(t);
  BaseTexture* old = d->st.textures[stage];
  d->st.textures[stage] = t;
  release_ref(old);
  rec().op(cmd::OpSetTexture, {stage, texture_id(t)});
}

void set_transform(Device* d, uint32_t s, const Mat4& m) {
  d->st.transforms[s] = m;
  rec().u32(cmd::OpSetTransform);
  rec().u32(s);
  for (float f : m.m) rec().f32(f);
}

const Mat4& get_transform(Device* d, uint32_t s) {
  auto it = d->st.transforms.find(s);
  static const Mat4 ident = Mat4::identity();
  return it == d->st.transforms.end() ? ident : it->second;
}

void set_viewport(Device* d, const Viewport& v) {
  d->st.viewport = v;
  rec().u32(cmd::OpSetViewport);
  rec().u32(v.x);
  rec().u32(v.y);
  rec().u32(v.w);
  rec().u32(v.h);
  rec().f32(v.min_z);
  rec().f32(v.max_z);
}

void set_material(Device* d, const Material& m) {
  d->st.material = m;
  rec().u32(cmd::OpSetMaterial);
  rec().bytes(&m, 17 * 4);
}

void set_light(Device* d, uint32_t i, const Light& l) {
  d->st.lights[i] = l;
  rec().u32(cmd::OpSetLight);
  rec().u32(i);
  rec().u32(l.type);
  rec().bytes(&l.diffuse, 25 * 4);
}

void light_enable(Device* d, uint32_t i, bool e) {
  if (!d->st.lights.count(i)) set_light(d, i, Light{});
  d->st.light_enable[i] = e;
  rec().op(cmd::OpLightEnable, {i, e ? 1u : 0u});
}

void emit_layout(const std::vector<VertexElement>& els) {
  rec().u32(cmd::OpSetVertexLayout);
  rec().u32((uint32_t)els.size());
  for (auto& e : els) {
    rec().u32(e.stream);
    rec().u32(e.offset);
    rec().u32(e.type);
    rec().u32(e.usage);
    rec().u32(e.usage_index);
  }
}

}  // namespace

std::vector<VertexElement> fvf_elements(uint32_t fvf) {
  std::vector<VertexElement> e;
  uint16_t off = 0;
  auto add = [&](uint8_t type, uint8_t usage, uint8_t idx, uint16_t size) {
    e.push_back({0, off, type, 0, usage, idx});
    off = (uint16_t)(off + size);
  };
  uint32_t pos = fvf & 0x400e;
  uint32_t weights = 0;
  if (pos == 0x004) add(3, 9, 0, 16);  // XYZRHW -> POSITIONT float4
  else if (pos == 0x4002) add(3, 0, 0, 16);  // XYZW
  else if (pos) {
    add(2, 0, 0, 12);
    weights = pos >= 0x006 ? (pos - 0x004) / 2 : 0;  // XYZB1..XYZB5
  }
  if (weights) {
    bool last_ubyte = (fvf & 0x1000) || (fvf & 0x8000);
    uint32_t n = last_ubyte ? weights - 1 : weights;
    if (n) add((uint8_t)(n - 1), 1, 0, (uint16_t)(4 * n));
    if (last_ubyte) add((fvf & 0x8000) ? 4 : 5, 2, 0, 4);
  }
  if (fvf & 0x010) add(2, 3, 0, 12);  // NORMAL
  if (fvf & 0x020) add(0, 4, 0, 4);   // PSIZE
  if (fvf & 0x040) add(4, 10, 0, 4);  // DIFFUSE
  if (fvf & 0x080) add(4, 10, 1, 4);  // SPECULAR
  uint32_t ntex = (fvf >> 8) & 0xf;
  for (uint32_t t = 0; t < ntex; t++) {
    uint32_t sz = (fvf >> (16 + 2 * t)) & 3;
    static const uint8_t type[4] = {1, 2, 3, 0};
    static const uint16_t bytes[4] = {8, 12, 16, 4};
    add(type[sz], 5, (uint8_t)t, bytes[sz]);
  }
  return e;
}

uint32_t fvf_stride(uint32_t fvf) {
  auto els = fvf_elements(fvf);
  static const uint16_t sizes[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
  uint32_t end = 0;
  for (auto& e : els) end = std::max<uint32_t>(end, e.offset + (e.type < 17 ? sizes[e.type] : 0));
  return end;
}

namespace {

void set_fvf(Device* d, uint32_t fvf) {
  d->st.fvf = fvf;
  d->st.decl = nullptr;
  emit_layout(fvf_elements(fvf));
}

void set_decl(Device* d, VertexDeclaration* decl) {
  d->st.decl = decl;
  if (decl) {
    d->st.fvf = decl->fvf;
    emit_layout(decl->elements);
  }
}

void emit_shaders(Device* d) {
  rec().op(cmd::OpSetShaders, {d->st.vs ? d->st.vs->program_id : 0, d->st.ps ? d->st.ps->program_id : 0});
}

void set_stream(Device* d, uint32_t i, VertexBuffer* vb, uint32_t off, uint32_t stride) {
  if (i >= 16) return;
  add_ref(vb);
  VertexBuffer* old = d->st.streams[i].vb;
  d->st.streams[i] = {vb, off, stride};
  release_ref(old);
  rec().op(cmd::OpSetStream, {i, vb ? vb->gl_buf : 0, off, stride});
}

void set_indices(Device* d, IndexBuffer* ib) {
  add_ref(ib);
  IndexBuffer* old = d->st.indices;
  d->st.indices = ib;
  release_ref(old);
  rec().op(cmd::OpSetIndices, {ib ? ib->gl_buf : 0, ib && ib->fvf_or_format == FMT_INDEX32 ? 32u : 16u});
}

void set_vsc(Device* d, uint32_t start, const float* data, uint32_t count) {
  if (start >= 256) return;
  count = std::min(count, 256 - start);
  memcpy(&d->st.vs_f[start * 4], data, count * 16);
  rec().op(cmd::OpSetVSConstF, {start, count});
  rec().bytes(data, count * 16);
}

void set_psc(Device* d, uint32_t start, const float* data, uint32_t count) {
  if (start >= 224) return;
  count = std::min(count, 224 - start);
  memcpy(&d->st.ps_f[start * 4], data, count * 16);
  rec().op(cmd::OpSetPSConstF, {start, count});
  rec().bytes(data, count * 16);
}

void set_scissor(Device* d, int32_t l, int32_t t, int32_t r, int32_t b) {
  d->st.scissor = {l, t, r, b};
  rec().op(cmd::OpSetScissor, {(uint32_t)l, (uint32_t)t, (uint32_t)r, (uint32_t)b});
}

void set_clip_plane(Device* d, uint32_t i, const float* p) {
  if (i >= 6) return;
  memcpy(d->st.clip_planes[i].data(), p, 16);
  rec().op(cmd::OpSetClipPlane, {i});
  rec().bytes(p, 16);
}

void emit_render_target(uint32_t index, Surface* s) {
  SurfaceRef r = s ? surface_ref(s) : SurfaceRef{0, 0, 0, 0};
  rec().op(cmd::OpSetRenderTarget, {index, r.kind, r.id, r.face, r.level});
}

void emit_depth(Surface* s) {
  SurfaceRef r = s ? surface_ref(s) : SurfaceRef{0, 0, 0, 0};
  rec().op(cmd::OpSetDepthStencil, {r.kind, r.id});
}

// Re-emit the whole state (after Reset or when applying a full state block).
void apply_state(Device* d, const State& s, bool vertex, bool pixel) {
  for (uint32_t i = 0; i < 256; i++)
    if (d->st.rs[i] != s.rs[i]) set_rs(d, i, s.rs[i]);
  if (pixel)
    for (uint32_t t = 0; t < 8; t++)
      for (uint32_t k = 0; k < 33; k++)
        if (d->st.tss[t][k] != s.tss[t][k]) set_tss(d, t, k, s.tss[t][k]);
  if (pixel)
    for (uint32_t t = 0; t < 20; t++)
      for (uint32_t k = 0; k < 14; k++)
        if (d->st.ss[t][k] != s.ss[t][k]) set_ss(d, t, k, s.ss[t][k]);
  if (vertex) {
    for (auto& [k, m] : s.transforms) set_transform(d, k, m);
    for (auto& [i, l] : s.lights) set_light(d, i, l);
    for (auto& [i, e] : s.light_enable) light_enable(d, i, e);
    set_material(d, s.material);
    for (uint32_t i = 0; i < 6; i++) set_clip_plane(d, i, s.clip_planes[i].data());
  }
}

// ---- IUnknown / misc -------------------------------------------------------------------------

uint32_t QueryInterface(Cpu* c, ComObject*) {
  st32(arg(c, 2), arg(c, 0));
  return S_OK;
}

uint32_t GetAvailableTextureMem(Cpu*, ComObject*) { return 256u << 20; }

uint32_t GetDirect3D(Cpu* c, ComObject* self) {
  uint32_t p = D(self)->d3d_ptr;
  if (ComObject* o = com_object(p)) o->refs++;
  st32(arg(c, 1), p);
  return S_OK;
}

uint32_t GetDeviceCaps(Cpu* c, ComObject*) {
  fill_caps(arg(c, 1));
  return S_OK;
}

uint32_t GetDisplayMode(Cpu* c, ComObject* self) {
  uint32_t out = arg(c, 2);
  st32(out, D(self)->pp[0]);
  st32(out + 4, D(self)->pp[1]);
  st32(out + 8, 60);
  st32(out + 12, D(self)->pp[2]);
  return S_OK;
}

uint32_t GetCreationParameters(Cpu* c, ComObject* self) {
  uint32_t p = arg(c, 1);
  st32(p, 0);
  st32(p + 4, 1);
  st32(p + 8, D(self)->hwnd);
  st32(p + 12, D(self)->behavior);
  return S_OK;
}

uint32_t ShowCursor(Cpu*, ComObject*) { return 0; }

uint32_t GetSwapChain(Cpu* c, ComObject* self) {
  auto* sc = new SwapChain();
  sc->device = D(self);
  st32(arg(c, 2), expose(sc, iface_swap_chain()));
  return S_OK;
}

uint32_t GetNumberOfSwapChains(Cpu*, ComObject*) { return 1; }

uint32_t Reset(Cpu* c, ComObject* self) {
  Device* d = D(self);
  uint32_t pp = arg(c, 1);
  for (int i = 0; i < 14; i++) d->pp[i] = ld32(pp + 4 * (uint32_t)i);
  log(Log::Info, "Device::Reset: %ux%u", d->pp[0], d->pp[1]);
  recorder_create_device(d);  // recreates back buffer / depth and default state
  return S_OK;
}

uint32_t Present(Cpu*, ComObject* self) {
  Device* d = D(self);
  rec().op(cmd::OpPresent, {(uint32_t)d->frame});
  d->frame++;
  if (trace_enabled("frames") || d->frame <= 3 || d->frame % 600 == 0)
    log(Log::Info, "Present #%llu (%u draws, %u primitives)", (unsigned long long)d->frame, d->draws, d->prims);
  d->draws = d->prims = 0;
  flush();
  return S_OK;
}

uint32_t GetBackBuffer(Cpu* c, ComObject* self) {
  Device* d = D(self);
  add_ref(d->back_buffer);
  st32(arg(c, 4), guest_ptr_of(d->back_buffer));
  return S_OK;
}

uint32_t GetRasterStatus(Cpu* c, ComObject*) {
  st32(arg(c, 2), 0);
  st32(arg(c, 2) + 4, 0);
  return S_OK;
}

uint32_t SetGammaRamp(Cpu* c, ComObject* self) {
  Device* d = D(self);
  uint32_t ramp = arg(c, 3);
  for (uint32_t i = 0; i < 3 * 256; i++) d->gamma[i] = ld16(ramp + 2 * i);
  rec().u32(cmd::OpSetGammaRamp);
  for (uint32_t i = 0; i < 3 * 256; i++) rec().u32(d->gamma[i]);
  return 0;
}

uint32_t GetGammaRamp(Cpu* c, ComObject* self) {
  for (uint32_t i = 0; i < 3 * 256; i++) st16(arg(c, 2) + 2 * i, D(self)->gamma[i]);
  return 0;
}

// ---- Resource creation -----------------------------------------------------------------------

uint32_t CreateTexture(Cpu* c, ComObject* self) {
  uint32_t w = arg(c, 1), h = arg(c, 2), levels = arg(c, 3), usage = arg(c, 4), fmt = arg(c, 5), pool = arg(c, 6), out = arg(c, 7);
  if (!format_info(fmt).known) {
    log(Log::Warn, "CreateTexture: unsupported format %08x", fmt);
    return D3DERR_INVALIDCALL;
  }
  BaseTexture* t = create_texture(D(self), false, w, h, levels, usage, fmt, pool);
  st32(out, expose(t, iface_texture()));
  return S_OK;
}

uint32_t CreateCubeTexture(Cpu* c, ComObject* self) {
  uint32_t edge = arg(c, 1), levels = arg(c, 2), usage = arg(c, 3), fmt = arg(c, 4), pool = arg(c, 5), out = arg(c, 6);
  BaseTexture* t = create_texture(D(self), true, edge, edge, levels, usage, fmt, pool);
  st32(out, expose(t, iface_cube_texture()));
  return S_OK;
}

uint32_t CreateVertexBuffer(Cpu* c, ComObject* self) {
  auto* vb = new VertexBuffer();
  vb->device = D(self);
  vb->length = arg(c, 1);
  vb->usage = arg(c, 2);
  vb->fvf_or_format = arg(c, 3);
  vb->pool = arg(c, 4);
  vb->mem = heap_alloc(process_heap(), vb->length, true);
  vb->gl_buf = new_resource_id();
  rec().op(cmd::OpCreateBuffer, {vb->gl_buf, 0, vb->length});
  st32(arg(c, 5), expose(vb, iface_vertex_buffer()));
  return S_OK;
}

uint32_t CreateIndexBuffer(Cpu* c, ComObject* self) {
  auto* ib = new IndexBuffer();
  ib->device = D(self);
  ib->length = arg(c, 1);
  ib->usage = arg(c, 2);
  ib->fvf_or_format = arg(c, 3);
  ib->pool = arg(c, 4);
  ib->mem = heap_alloc(process_heap(), ib->length, true);
  ib->gl_buf = new_resource_id();
  rec().op(cmd::OpCreateBuffer, {ib->gl_buf, 1, ib->length});
  st32(arg(c, 5), expose(ib, iface_index_buffer()));
  return S_OK;
}

uint32_t CreateRenderTarget(Cpu* c, ComObject* self) {
  Surface* s = create_surface(D(self), arg(c, 1), arg(c, 2), arg(c, 3), USAGE_RENDERTARGET, POOL_DEFAULT);
  st32(arg(c, 7), expose(s, iface_surface()));
  return S_OK;
}

uint32_t CreateDepthStencilSurface(Cpu* c, ComObject* self) {
  Surface* s = create_surface(D(self), arg(c, 1), arg(c, 2), arg(c, 3), USAGE_DEPTHSTENCIL, POOL_DEFAULT);
  st32(arg(c, 7), expose(s, iface_surface()));
  return S_OK;
}

uint32_t CreateOffscreenPlainSurface(Cpu* c, ComObject* self) {
  Surface* s = create_surface(D(self), arg(c, 1), arg(c, 2), arg(c, 3), 0, arg(c, 4));
  st32(arg(c, 5), expose(s, iface_surface()));
  return S_OK;
}

// Copy a rectangle of shadow memory between two surfaces of the same format.
void copy_rect(Surface* src, int sx, int sy, Surface* dst, int dx, int dy, int w, int h) {
  FormatInfo f = format_info(src->format);
  uint32_t s = src->ensure_memory(), d = dst->ensure_memory();
  if (f.block) {
    sx /= 4, sy /= 4, dx /= 4, dy /= 4;
    w = (w + 3) / 4, h = (h + 3) / 4;
    for (int y = 0; y < h; y++)
      memcpy(host(d + (uint32_t)(dy + y) * dst->pitch + (uint32_t)dx * f.block), host(s + (uint32_t)(sy + y) * src->pitch + (uint32_t)sx * f.block),
             (size_t)w * f.block);
  } else {
    uint32_t bpp = f.bpp / 8;
    for (int y = 0; y < h; y++)
      memcpy(host(d + (uint32_t)(dy + y) * dst->pitch + (uint32_t)dx * bpp), host(s + (uint32_t)(sy + y) * src->pitch + (uint32_t)sx * bpp),
             (size_t)w * bpp);
  }
}

uint32_t UpdateSurface(Cpu* c, ComObject*) {
  Surface* src = as<Surface>(arg(c, 1));
  Surface* dst = as<Surface>(arg(c, 3));
  if (!src || !dst) return D3DERR_INVALIDCALL;
  int sx = 0, sy = 0, w = (int)src->width, h = (int)src->height, dx = 0, dy = 0;
  if (arg(c, 2)) {
    sx = (int)ld32(arg(c, 2)), sy = (int)ld32(arg(c, 2) + 4);
    w = (int)ld32(arg(c, 2) + 8) - sx, h = (int)ld32(arg(c, 2) + 12) - sy;
  }
  if (arg(c, 4)) dx = (int)ld32(arg(c, 4)), dy = (int)ld32(arg(c, 4) + 4);
  copy_rect(src, sx, sy, dst, dx, dy, w, h);
  upload_surface(dst);
  return S_OK;
}

uint32_t UpdateTexture(Cpu* c, ComObject*) {
  BaseTexture* src = as<BaseTexture>(arg(c, 1));
  BaseTexture* dst = as<BaseTexture>(arg(c, 2));
  if (!src || !dst) return D3DERR_INVALIDCALL;
  uint32_t n = std::min(src->surfaces.size(), dst->surfaces.size());
  for (uint32_t i = 0; i < n; i++) {
    Surface* s = src->surfaces[i];
    Surface* d = dst->surfaces[i];
    copy_rect(s, 0, 0, d, 0, 0, (int)std::min(s->width, d->width), (int)std::min(s->height, d->height));
    upload_surface(d);
  }
  return S_OK;
}

uint32_t GetRenderTargetData(Cpu* c, ComObject*) {
  Surface* src = as<Surface>(arg(c, 1));
  Surface* dst = as<Surface>(arg(c, 2));
  if (!src || !dst) return D3DERR_INVALIDCALL;
  SurfaceRef r = surface_ref(src);
  uint32_t mem = dst->ensure_memory();
  rec().op(cmd::OpReadback, {r.kind, r.id, r.face, r.level, src->width, src->height, src->format, mem, dst->pitch});
  flush_and_wait();
  return S_OK;
}

uint32_t StretchRect(Cpu* c, ComObject*) {
  Surface* src = as<Surface>(arg(c, 1));
  Surface* dst = as<Surface>(arg(c, 3));
  if (!src || !dst) return D3DERR_INVALIDCALL;
  auto rect = [](uint32_t p, Surface* s, uint32_t* r) {
    if (p) for (int i = 0; i < 4; i++) r[i] = ld32(p + 4 * (uint32_t)i);
    else r[0] = r[1] = 0, r[2] = s->width, r[3] = s->height;
  };
  uint32_t sr[4], dr[4];
  rect(arg(c, 2), src, sr);
  rect(arg(c, 4), dst, dr);
  SurfaceRef a = surface_ref(src), b = surface_ref(dst);
  if (a.kind == 0) {
    // Source has only shadow memory (e.g. an offscreen plain surface): upload it into a scratch texture.
    upload_surface(src);
    a = surface_ref(src);
  }
  rec().op(cmd::OpStretchRect, {a.kind, a.id, a.face, a.level, sr[0], sr[1], sr[2], sr[3], b.kind, b.id, b.face, b.level, dr[0], dr[1], dr[2], dr[3], arg(c, 5)});
  return S_OK;
}

uint32_t ColorFill(Cpu* c, ComObject*) {
  Surface* s = as<Surface>(arg(c, 1));
  if (!s) return D3DERR_INVALIDCALL;
  uint32_t r[4] = {0, 0, s->width, s->height};
  if (arg(c, 2))
    for (int i = 0; i < 4; i++) r[i] = ld32(arg(c, 2) + 4 * (uint32_t)i);
  SurfaceRef ref = surface_ref(s);
  rec().op(cmd::OpColorFill, {ref.kind, ref.id, ref.face, ref.level, r[0], r[1], r[2], r[3], arg(c, 3)});
  return S_OK;
}

uint32_t SetRenderTarget(Cpu* c, ComObject* self) {
  Device* d = D(self);
  uint32_t i = arg(c, 1);
  Surface* s = as<Surface>(arg(c, 2));
  if (i >= 4 || (i == 0 && !s)) return D3DERR_INVALIDCALL;
  addref(s);
  Surface* old = d->render_targets[i];
  d->render_targets[i] = s;
  release_ref(old);
  emit_render_target(i, s);
  if (i == 0 && s) {
    // Setting render target 0 resets the viewport (and scissor) to the full target.
    set_viewport(d, Viewport{0, 0, s->width, s->height, 0.0f, 1.0f});
    set_scissor(d, 0, 0, (int32_t)s->width, (int32_t)s->height);
  }
  return S_OK;
}

uint32_t GetRenderTarget(Cpu* c, ComObject* self) {
  Device* d = D(self);
  uint32_t i = arg(c, 1);
  Surface* s = i < 4 ? d->render_targets[i] : nullptr;
  if (!s) {
    st32(arg(c, 2), 0);
    return D3DERR_NOTFOUND;
  }
  add_ref(s);
  st32(arg(c, 2), guest_ptr_of(s));
  return S_OK;
}

uint32_t SetDepthStencilSurface(Cpu* c, ComObject* self) {
  Device* d = D(self);
  Surface* s = as<Surface>(arg(c, 1));
  addref(s);
  Surface* old = d->depth_stencil;
  d->depth_stencil = s;
  release_ref(old);
  emit_depth(s);
  return S_OK;
}

uint32_t GetDepthStencilSurface(Cpu* c, ComObject* self) {
  Surface* s = D(self)->depth_stencil;
  if (!s) {
    st32(arg(c, 1), 0);
    return D3DERR_NOTFOUND;
  }
  add_ref(s);
  st32(arg(c, 1), guest_ptr_of(s));
  return S_OK;
}

uint32_t BeginScene(Cpu*, ComObject* self) {
  D(self)->in_scene = true;
  return S_OK;
}

uint32_t EndScene(Cpu*, ComObject* self) {
  D(self)->in_scene = false;
  return S_OK;
}

uint32_t Clear(Cpu* c, ComObject*) {
  uint32_t n = arg(c, 1), rects = arg(c, 2);
  rec().op(cmd::OpClear, {arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), rects ? n : 0});
  if (rects)
    for (uint32_t i = 0; i < n * 4; i++) rec().u32(ld32(rects + 4 * i));
  return S_OK;
}

Mat4 read_mat(uint32_t p) {
  Mat4 m;
  memcpy(m.m, host(p), 64);
  return m;
}

uint32_t SetTransform(Cpu* c, ComObject* self) {
  Device* d = D(self);
  Mat4 m = read_mat(arg(c, 2));
  if (recording(d, {RK_XFORM, arg(c, 1), 0, std::vector<uint32_t>((uint32_t*)m.m, (uint32_t*)m.m + 16)})) return S_OK;
  set_transform(d, arg(c, 1), m);
  return S_OK;
}

uint32_t GetTransform(Cpu* c, ComObject* self) {
  memcpy(host(arg(c, 2)), get_transform(D(self), arg(c, 1)).m, 64);
  return S_OK;
}

uint32_t MultiplyTransform(Cpu* c, ComObject* self) {
  Device* d = D(self);
  Mat4 a = get_transform(d, arg(c, 1)), b = read_mat(arg(c, 2)), r{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++) r.m[i * 4 + j] += b.m[i * 4 + k] * a.m[k * 4 + j];
  set_transform(d, arg(c, 1), r);
  return S_OK;
}

Viewport read_viewport(uint32_t p) {
  return Viewport{ld32(p), ld32(p + 4), ld32(p + 8), ld32(p + 12), fl(ld32(p + 16)), fl(ld32(p + 20))};
}

uint32_t SetViewport(Cpu* c, ComObject* self) {
  Device* d = D(self);
  Viewport v = read_viewport(arg(c, 1));
  if (recording(d, {RK_VP, 0, 0, {v.x, v.y, v.w, v.h, f32bits(v.min_z), f32bits(v.max_z)}})) return S_OK;
  set_viewport(d, v);
  return S_OK;
}

uint32_t GetViewport(Cpu* c, ComObject* self) {
  const Viewport& v = D(self)->st.viewport;
  uint32_t p = arg(c, 1);
  st32(p, v.x);
  st32(p + 4, v.y);
  st32(p + 8, v.w);
  st32(p + 12, v.h);
  st32(p + 16, f32bits(v.min_z));
  st32(p + 20, f32bits(v.max_z));
  return S_OK;
}

uint32_t SetMaterial(Cpu* c, ComObject* self) {
  Device* d = D(self);
  Material m;
  memcpy(&m, host(arg(c, 1)), 68);
  if (recording(d, {RK_MAT, 0, 0, std::vector<uint32_t>((uint32_t*)&m, (uint32_t*)&m + 17)})) return S_OK;
  set_material(d, m);
  return S_OK;
}

uint32_t GetMaterial(Cpu* c, ComObject* self) {
  memcpy(host(arg(c, 1)), &D(self)->st.material, 68);
  return S_OK;
}

uint32_t SetLight(Cpu* c, ComObject* self) {
  Device* d = D(self);
  Light l;
  uint32_t p = arg(c, 2);
  l.type = ld32(p);
  memcpy(&l.diffuse, host(p + 4), 25 * 4);
  if (recording(d, {RK_LIGHT, arg(c, 1), 0, std::vector<uint32_t>((uint32_t*)host(p), (uint32_t*)host(p) + 26)})) return S_OK;
  set_light(d, arg(c, 1), l);
  return S_OK;
}

uint32_t GetLight(Cpu* c, ComObject* self) {
  auto& lights = D(self)->st.lights;
  auto it = lights.find(arg(c, 1));
  if (it == lights.end()) return D3DERR_INVALIDCALL;
  st32(arg(c, 2), it->second.type);
  memcpy(host(arg(c, 2) + 4), &it->second.diffuse, 25 * 4);
  return S_OK;
}

uint32_t LightEnable(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_LIGHTEN, arg(c, 1), arg(c, 2), {}})) return S_OK;
  light_enable(d, arg(c, 1), arg(c, 2) != 0);
  return S_OK;
}

uint32_t GetLightEnable(Cpu* c, ComObject* self) {
  auto& e = D(self)->st.light_enable;
  auto it = e.find(arg(c, 1));
  st32(arg(c, 2), it != e.end() && it->second ? 128 : 0);
  return S_OK;
}

uint32_t SetClipPlane(Cpu* c, ComObject* self) {
  float p[4];
  memcpy(p, host(arg(c, 2)), 16);
  set_clip_plane(D(self), arg(c, 1), p);
  return S_OK;
}

uint32_t GetClipPlane(Cpu* c, ComObject* self) {
  uint32_t i = arg(c, 1);
  if (i >= 6) return D3DERR_INVALIDCALL;
  memcpy(host(arg(c, 2)), D(self)->st.clip_planes[i].data(), 16);
  return S_OK;
}

uint32_t SetRenderState(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_RS, arg(c, 1), arg(c, 2), {}})) return S_OK;
  set_rs(d, arg(c, 1), arg(c, 2));
  return S_OK;
}

uint32_t GetRenderState(Cpu* c, ComObject* self) {
  uint32_t s = arg(c, 1);
  st32(arg(c, 2), s < 256 ? D(self)->st.rs[s] : 0);
  return S_OK;
}

uint32_t CreateStateBlock(Cpu* c, ComObject* self) {
  auto* sb = new StateBlock();
  sb->device = D(self);
  sb->snapshot = D(self)->st;
  st32(arg(c, 2), expose(sb, iface_state_block()));
  return S_OK;
}

uint32_t BeginStateBlock(Cpu*, ComObject* self) {
  auto* sb = new StateBlock();
  sb->device = D(self);
  sb->recorded = true;
  D(self)->recording = sb;
  g_recorded[sb].clear();
  return S_OK;
}

uint32_t EndStateBlock(Cpu* c, ComObject* self) {
  Device* d = D(self);
  StateBlock* sb = d->recording;
  if (!sb) return D3DERR_INVALIDCALL;
  d->recording = nullptr;
  st32(arg(c, 1), expose(sb, iface_state_block()));
  return S_OK;
}

uint32_t SetClipStatus(Cpu*, ComObject*) { return S_OK; }
uint32_t GetClipStatus(Cpu* c, ComObject*) {
  st32(arg(c, 1), 0);
  st32(arg(c, 1) + 4, 0);
  return S_OK;
}

uint32_t GetTexture(Cpu* c, ComObject* self) {
  uint32_t i = sampler_index(arg(c, 1));
  BaseTexture* t = i < 20 ? D(self)->st.textures[i] : nullptr;
  if (t) t->refs++;
  st32(arg(c, 2), t ? guest_ptr_of(t) : 0);
  return S_OK;
}

uint32_t SetTexture(Cpu* c, ComObject* self) {
  Device* d = D(self);
  BaseTexture* t = as<BaseTexture>(arg(c, 2));
  if (recording(d, {RK_TEX, arg(c, 1), arg(c, 2), {}})) return S_OK;
  set_texture(d, arg(c, 1), t);
  return S_OK;
}

uint32_t GetTextureStageState(Cpu* c, ComObject* self) {
  uint32_t s = arg(c, 1), t = arg(c, 2);
  st32(arg(c, 3), s < 8 && t < 33 ? D(self)->st.tss[s][t] : 0);
  return S_OK;
}

uint32_t SetTextureStageState(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_TSS, arg(c, 1), arg(c, 2), {arg(c, 3)}})) return S_OK;
  set_tss(d, arg(c, 1), arg(c, 2), arg(c, 3));
  return S_OK;
}

uint32_t GetSamplerState(Cpu* c, ComObject* self) {
  uint32_t s = sampler_index(arg(c, 1)), t = arg(c, 2);
  st32(arg(c, 3), s < 20 && t < 14 ? D(self)->st.ss[s][t] : 0);
  return S_OK;
}

uint32_t SetSamplerState(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_SS, arg(c, 1), arg(c, 2), {arg(c, 3)}})) return S_OK;
  set_ss(d, arg(c, 1), arg(c, 2), arg(c, 3));
  return S_OK;
}

uint32_t ValidateDevice(Cpu* c, ComObject*) {
  if (arg(c, 1)) st32(arg(c, 1), 1);
  return S_OK;
}

uint32_t SetScissorRect(Cpu* c, ComObject* self) {
  uint32_t r = arg(c, 1);
  set_scissor(D(self), (int32_t)ld32(r), (int32_t)ld32(r + 4), (int32_t)ld32(r + 8), (int32_t)ld32(r + 12));
  return S_OK;
}

uint32_t GetScissorRect(Cpu* c, ComObject* self) {
  auto& s = D(self)->st.scissor;
  uint32_t r = arg(c, 1);
  st32(r, (uint32_t)s.left);
  st32(r + 4, (uint32_t)s.top);
  st32(r + 8, (uint32_t)s.right);
  st32(r + 12, (uint32_t)s.bottom);
  return S_OK;
}

uint32_t GetSoftwareVertexProcessing(Cpu*, ComObject*) { return 0; }
uint32_t GetNPatchMode(Cpu*, ComObject*) { return 0; }

uint32_t prim_vertex_count(uint32_t prim, uint32_t n) {
  switch (prim) {
    case 1: return n;          // POINTLIST
    case 2: return n * 2;      // LINELIST
    case 3: return n + 1;      // LINESTRIP
    case 4: return n * 3;      // TRIANGLELIST
    case 5: return n + 2;      // TRIANGLESTRIP
    case 6: return n + 2;      // TRIANGLEFAN
    default: return 0;
  }
}

uint32_t DrawPrimitive(Cpu* c, ComObject* self) {
  Device* d = D(self);
  d->draws++;
  d->prims += arg(c, 3);
  rec().op(cmd::OpDraw, {arg(c, 1), arg(c, 2), arg(c, 3)});
  return S_OK;
}

uint32_t DrawIndexedPrimitive(Cpu* c, ComObject* self) {
  Device* d = D(self);
  d->draws++;
  d->prims += arg(c, 6);
  rec().op(cmd::OpDrawIndexed, {arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6)});
  return S_OK;
}

uint32_t DrawPrimitiveUP(Cpu* c, ComObject* self) {
  Device* d = D(self);
  uint32_t prim = arg(c, 1), n = arg(c, 2), data = arg(c, 3), stride = arg(c, 4);
  uint32_t bytes = prim_vertex_count(prim, n) * stride;
  d->draws++;
  d->prims += n;
  rec().op(cmd::OpDrawUP, {prim, n, stride, bytes});
  rec().bytes(host(data), bytes);
  // D3D9 unbinds stream 0 after *UP draws.
  release_ref(d->st.streams[0].vb);
  d->st.streams[0] = {};
  return S_OK;
}

uint32_t DrawIndexedPrimitiveUP(Cpu* c, ComObject* self) {
  Device* d = D(self);
  uint32_t prim = arg(c, 1), minv = arg(c, 2), nverts = arg(c, 3), n = arg(c, 4), idx = arg(c, 5), fmt = arg(c, 6), data = arg(c, 7),
           stride = arg(c, 8);
  uint32_t isize = fmt == FMT_INDEX32 ? 4 : 2;
  uint32_t ibytes = prim_vertex_count(prim, n) * isize;
  uint32_t vbytes = (minv + nverts) * stride;
  d->draws++;
  d->prims += n;
  rec().op(cmd::OpDrawIndexedUP, {prim, minv, nverts, n, isize * 8, stride, vbytes, ibytes});
  rec().bytes(host(data), vbytes);
  rec().bytes(host(idx), ibytes);
  release_ref(d->st.streams[0].vb);
  d->st.streams[0] = {};
  release_ref(d->st.indices);
  d->st.indices = nullptr;
  return S_OK;
}

uint32_t CreateVertexDeclaration(Cpu* c, ComObject* self) {
  auto* decl = new VertexDeclaration();
  decl->device = D(self);
  for (uint32_t p = arg(c, 1);; p += 8) {
    uint16_t stream = (uint16_t)ld16(p);
    uint8_t type = ld8(p + 4);
    if (stream == 0xff || type == 17) break;
    decl->elements.push_back({stream, (uint16_t)ld16(p + 2), type, (uint8_t)ld8(p + 5), (uint8_t)ld8(p + 6), (uint8_t)ld8(p + 7)});
  }
  st32(arg(c, 2), expose(decl, iface_vertex_decl()));
  return S_OK;
}

uint32_t SetVertexDeclaration(Cpu* c, ComObject* self) {
  Device* d = D(self);
  auto* decl = as<VertexDeclaration>(arg(c, 1));
  if (recording(d, {RK_DECL, arg(c, 1), 0, {}})) return S_OK;
  set_decl(d, decl);
  return S_OK;
}

uint32_t GetVertexDeclaration(Cpu* c, ComObject* self) {
  VertexDeclaration* decl = D(self)->st.decl;
  if (decl) decl->refs++;
  st32(arg(c, 1), decl ? guest_ptr_of(decl) : 0);
  return S_OK;
}

uint32_t SetFVF(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_FVF, arg(c, 1), 0, {}})) return S_OK;
  set_fvf(d, arg(c, 1));
  return S_OK;
}

uint32_t GetFVF(Cpu* c, ComObject* self) {
  st32(arg(c, 1), D(self)->st.fvf);
  return S_OK;
}

std::vector<uint32_t> read_shader_tokens(uint32_t p) {
  std::vector<uint32_t> t;
  for (uint32_t i = 0; i < 65536; i++) {
    uint32_t tok = ld32(p + 4 * i);
    t.push_back(tok);
    if (i > 0 && (tok & 0xffff) == 0xfffe) {  // comment: skip its payload
      uint32_t len = (tok >> 16) & 0x7fff;
      for (uint32_t k = 0; k < len; k++) t.push_back(ld32(p + 4 * (i + 1 + k)));
      i += len;
      continue;
    }
    if (tok == 0x0000ffff) break;
  }
  return t;
}

uint32_t create_shader(Cpu* c, ComObject* self, bool pixel) {
  auto* s = new Shader();
  s->device = D(self);
  s->pixel = pixel;
  s->tokens = read_shader_tokens(arg(c, 1));
  s->program_id = new_resource_id();
  rec().op(cmd::OpCreateShader, {s->program_id, pixel ? 1u : 0u, (uint32_t)s->tokens.size()});
  rec().bytes(s->tokens.data(), (uint32_t)s->tokens.size() * 4);
  st32(arg(c, 2), expose(s, pixel ? iface_pixel_shader() : iface_vertex_shader()));
  return S_OK;
}

uint32_t CreateVertexShader(Cpu* c, ComObject* self) { return create_shader(c, self, false); }
uint32_t CreatePixelShader(Cpu* c, ComObject* self) { return create_shader(c, self, true); }

uint32_t SetVertexShader(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_VS, arg(c, 1), 0, {}})) return S_OK;
  d->st.vs = as<Shader>(arg(c, 1));
  emit_shaders(d);
  return S_OK;
}

uint32_t SetPixelShader(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_PS, arg(c, 1), 0, {}})) return S_OK;
  d->st.ps = as<Shader>(arg(c, 1));
  emit_shaders(d);
  return S_OK;
}

uint32_t GetVertexShader(Cpu* c, ComObject* self) {
  Shader* s = D(self)->st.vs;
  if (s) s->refs++;
  st32(arg(c, 1), s ? guest_ptr_of(s) : 0);
  return S_OK;
}

uint32_t GetPixelShader(Cpu* c, ComObject* self) {
  Shader* s = D(self)->st.ps;
  if (s) s->refs++;
  st32(arg(c, 1), s ? guest_ptr_of(s) : 0);
  return S_OK;
}

uint32_t SetVertexShaderConstantF(Cpu* c, ComObject* self) {
  set_vsc(D(self), arg(c, 1), (const float*)host(arg(c, 2)), arg(c, 3));
  return S_OK;
}

uint32_t GetVertexShaderConstantF(Cpu* c, ComObject* self) {
  uint32_t start = arg(c, 1), n = arg(c, 3);
  if (start + n > 256) return D3DERR_INVALIDCALL;
  memcpy(host(arg(c, 2)), &D(self)->st.vs_f[start * 4], n * 16);
  return S_OK;
}

uint32_t SetPixelShaderConstantF(Cpu* c, ComObject* self) {
  set_psc(D(self), arg(c, 1), (const float*)host(arg(c, 2)), arg(c, 3));
  return S_OK;
}

uint32_t GetPixelShaderConstantF(Cpu* c, ComObject* self) {
  uint32_t start = arg(c, 1), n = arg(c, 3);
  if (start + n > 224) return D3DERR_INVALIDCALL;
  memcpy(host(arg(c, 2)), &D(self)->st.ps_f[start * 4], n * 16);
  return S_OK;
}

uint32_t SetConstI(Cpu* c, ComObject* self, bool pixel) {
  uint32_t start = arg(c, 1), n = arg(c, 3);
  if (start + n > 16) return D3DERR_INVALIDCALL;
  auto& dst = pixel ? D(self)->st.ps_i : D(self)->st.vs_i;
  memcpy(&dst[start * 4], host(arg(c, 2)), n * 16);
  rec().op(pixel ? cmd::OpSetPSConstI : cmd::OpSetVSConstI, {start, n});
  rec().bytes(host(arg(c, 2)), n * 16);
  return S_OK;
}

uint32_t SetConstB(Cpu* c, ComObject* self, bool pixel) {
  uint32_t start = arg(c, 1), n = arg(c, 3);
  if (start + n > 16) return D3DERR_INVALIDCALL;
  auto& dst = pixel ? D(self)->st.ps_b : D(self)->st.vs_b;
  memcpy(&dst[start], host(arg(c, 2)), n * 4);
  rec().op(pixel ? cmd::OpSetPSConstB : cmd::OpSetVSConstB, {start, n});
  rec().bytes(host(arg(c, 2)), n * 4);
  return S_OK;
}

uint32_t SetVertexShaderConstantI(Cpu* c, ComObject* self) { return SetConstI(c, self, false); }
uint32_t SetPixelShaderConstantI(Cpu* c, ComObject* self) { return SetConstI(c, self, true); }
uint32_t SetVertexShaderConstantB(Cpu* c, ComObject* self) { return SetConstB(c, self, false); }
uint32_t SetPixelShaderConstantB(Cpu* c, ComObject* self) { return SetConstB(c, self, true); }

uint32_t SetStreamSource(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_STREAM, arg(c, 1), arg(c, 2), {arg(c, 3), arg(c, 4)}})) return S_OK;
  set_stream(d, arg(c, 1), as<VertexBuffer>(arg(c, 2)), arg(c, 3), arg(c, 4));
  return S_OK;
}

uint32_t GetStreamSource(Cpu* c, ComObject* self) {
  uint32_t i = arg(c, 1);
  if (i >= 16) return D3DERR_INVALIDCALL;
  Stream& s = D(self)->st.streams[i];
  if (s.vb) s.vb->refs++;
  st32(arg(c, 2), s.vb ? guest_ptr_of(s.vb) : 0);
  st32(arg(c, 3), s.offset);
  st32(arg(c, 4), s.stride);
  return S_OK;
}

uint32_t SetStreamSourceFreq(Cpu* c, ComObject* self) {
  if (arg(c, 1) < 16) D(self)->st.stream_freq[arg(c, 1)] = arg(c, 2);
  return S_OK;
}

uint32_t GetStreamSourceFreq(Cpu* c, ComObject* self) {
  st32(arg(c, 2), arg(c, 1) < 16 ? D(self)->st.stream_freq[arg(c, 1)] : 1);
  return S_OK;
}

uint32_t SetIndices(Cpu* c, ComObject* self) {
  Device* d = D(self);
  if (recording(d, {RK_IND, arg(c, 1), 0, {}})) return S_OK;
  set_indices(d, as<IndexBuffer>(arg(c, 1)));
  return S_OK;
}

uint32_t GetIndices(Cpu* c, ComObject* self) {
  IndexBuffer* ib = D(self)->st.indices;
  if (ib) ib->refs++;
  st32(arg(c, 1), ib ? guest_ptr_of(ib) : 0);
  return S_OK;
}

uint32_t CreateQuery(Cpu* c, ComObject* self) {
  uint32_t type = arg(c, 1), out = arg(c, 2);
  bool supported = type == 8 || type == 9;  // EVENT, OCCLUSION
  if (!out) return supported ? S_OK : D3DERR_NOTAVAILABLE;
  if (!supported) return D3DERR_NOTAVAILABLE;
  auto* q = new Query();
  q->device = D(self);
  q->type = type;
  st32(out, expose(q, iface_query()));
  return S_OK;
}

}  // namespace

// Apply a state block (used by StateBlock::Apply).
void state_block_apply(StateBlock* sb) {
  Device* d = sb->device;
  if (!sb->recorded) {
    apply_state(d, sb->snapshot, true, true);
    for (uint32_t i = 0; i < 20; i++) set_texture(d, i, sb->snapshot.textures[i]);
    set_viewport(d, sb->snapshot.viewport);
    return;
  }
  for (auto& op : g_recorded[sb]) {
    switch (op.kind) {
      case RK_RS: set_rs(d, op.a, op.b); break;
      case RK_TSS: set_tss(d, op.a, op.b, op.data[0]); break;
      case RK_SS: set_ss(d, op.a, op.b, op.data[0]); break;
      case RK_TEX: set_texture(d, op.a, op.b ? com_as<BaseTexture>(op.b) : nullptr); break;
      case RK_XFORM: {
        Mat4 m;
        memcpy(m.m, op.data.data(), 64);
        set_transform(d, op.a, m);
        break;
      }
      case RK_VP: set_viewport(d, Viewport{op.data[0], op.data[1], op.data[2], op.data[3], fl(op.data[4]), fl(op.data[5])}); break;
      case RK_MAT: {
        Material m;
        memcpy(&m, op.data.data(), 68);
        set_material(d, m);
        break;
      }
      case RK_LIGHT: {
        Light l;
        l.type = op.data[0];
        memcpy(&l.diffuse, op.data.data() + 1, 25 * 4);
        set_light(d, op.a, l);
        break;
      }
      case RK_LIGHTEN: light_enable(d, op.a, op.b != 0); break;
      case RK_FVF: set_fvf(d, op.a); break;
      case RK_DECL: set_decl(d, op.a ? com_as<VertexDeclaration>(op.a) : nullptr); break;
      case RK_VS: d->st.vs = op.a ? com_as<Shader>(op.a) : nullptr; emit_shaders(d); break;
      case RK_PS: d->st.ps = op.a ? com_as<Shader>(op.a) : nullptr; emit_shaders(d); break;
      case RK_STREAM: set_stream(d, op.a, op.b ? com_as<VertexBuffer>(op.b) : nullptr, op.data[0], op.data[1]); break;
      case RK_IND: set_indices(d, op.a ? com_as<IndexBuffer>(op.a) : nullptr); break;
      default: break;
    }
  }
}

void state_block_capture(StateBlock* sb) {
  if (!sb->recorded) sb->snapshot = sb->device->st;
  // Recorded blocks capture the current values of the states they contain.
  else
    for (auto& op : g_recorded[sb]) {
      State& s = sb->device->st;
      switch (op.kind) {
        case RK_RS: op.b = s.rs[op.a & 255]; break;
        case RK_TSS: op.data[0] = s.tss[op.a & 7][op.b % 33]; break;
        case RK_SS: op.data[0] = s.ss[sampler_index(op.a) % 20][op.b % 14]; break;
        default: break;
      }
    }
}

void state_block_forget(StateBlock* sb) { g_recorded.erase(sb); }

rt::ComInterface& iface_device() {
  static rt::ComInterface i(
      "IDirect3DDevice9",
      {{"QueryInterface", 2, QueryInterface}, {"AddRef", 0, com_AddRef}, {"Release", 0, com_Release},
       {"TestCooperativeLevel", 0, ok}, {"GetAvailableTextureMem", 0, GetAvailableTextureMem}, {"EvictManagedResources", 0, ok},
       {"GetDirect3D", 1, GetDirect3D}, {"GetDeviceCaps", 1, GetDeviceCaps}, {"GetDisplayMode", 2, GetDisplayMode},
       {"GetCreationParameters", 1, GetCreationParameters}, {"SetCursorProperties", 3, ok}, {"SetCursorPosition", 3, ok},
       {"ShowCursor", 1, ShowCursor}, {"CreateAdditionalSwapChain", 2, nullptr}, {"GetSwapChain", 2, GetSwapChain},
       {"GetNumberOfSwapChains", 0, GetNumberOfSwapChains}, {"Reset", 1, Reset}, {"Present", 4, Present},
       {"GetBackBuffer", 4, GetBackBuffer}, {"GetRasterStatus", 2, GetRasterStatus}, {"SetDialogBoxMode", 1, ok},
       {"SetGammaRamp", 3, SetGammaRamp}, {"GetGammaRamp", 2, GetGammaRamp}, {"CreateTexture", 8, CreateTexture},
       {"CreateVolumeTexture", 9, nullptr}, {"CreateCubeTexture", 7, CreateCubeTexture},
       {"CreateVertexBuffer", 6, CreateVertexBuffer}, {"CreateIndexBuffer", 6, CreateIndexBuffer},
       {"CreateRenderTarget", 8, CreateRenderTarget}, {"CreateDepthStencilSurface", 8, CreateDepthStencilSurface},
       {"UpdateSurface", 4, UpdateSurface}, {"UpdateTexture", 2, UpdateTexture}, {"GetRenderTargetData", 2, GetRenderTargetData},
       {"GetFrontBufferData", 2, nullptr}, {"StretchRect", 5, StretchRect}, {"ColorFill", 3, ColorFill},
       {"CreateOffscreenPlainSurface", 6, CreateOffscreenPlainSurface}, {"SetRenderTarget", 2, SetRenderTarget},
       {"GetRenderTarget", 2, GetRenderTarget}, {"SetDepthStencilSurface", 1, SetDepthStencilSurface},
       {"GetDepthStencilSurface", 1, GetDepthStencilSurface}, {"BeginScene", 0, BeginScene}, {"EndScene", 0, EndScene},
       {"Clear", 6, Clear}, {"SetTransform", 2, SetTransform}, {"GetTransform", 2, GetTransform},
       {"MultiplyTransform", 2, MultiplyTransform}, {"SetViewport", 1, SetViewport}, {"GetViewport", 1, GetViewport},
       {"SetMaterial", 1, SetMaterial}, {"GetMaterial", 1, GetMaterial}, {"SetLight", 2, SetLight}, {"GetLight", 2, GetLight},
       {"LightEnable", 2, LightEnable}, {"GetLightEnable", 2, GetLightEnable}, {"SetClipPlane", 2, SetClipPlane},
       {"GetClipPlane", 2, GetClipPlane}, {"SetRenderState", 2, SetRenderState}, {"GetRenderState", 2, GetRenderState},
       {"CreateStateBlock", 2, CreateStateBlock}, {"BeginStateBlock", 0, BeginStateBlock}, {"EndStateBlock", 1, EndStateBlock},
       {"SetClipStatus", 1, SetClipStatus}, {"GetClipStatus", 1, GetClipStatus}, {"GetTexture", 2, GetTexture},
       {"SetTexture", 2, SetTexture}, {"GetTextureStageState", 3, GetTextureStageState},
       {"SetTextureStageState", 3, SetTextureStageState}, {"GetSamplerState", 3, GetSamplerState},
       {"SetSamplerState", 3, SetSamplerState}, {"ValidateDevice", 1, ValidateDevice}, {"SetPaletteEntries", 2, ok},
       {"GetPaletteEntries", 2, ok}, {"SetCurrentTexturePalette", 1, ok}, {"GetCurrentTexturePalette", 1, ok},
       {"SetScissorRect", 1, SetScissorRect}, {"GetScissorRect", 1, GetScissorRect}, {"SetSoftwareVertexProcessing", 1, ok},
       {"GetSoftwareVertexProcessing", 0, GetSoftwareVertexProcessing}, {"SetNPatchMode", 1, ok},
       {"GetNPatchMode", 0, GetNPatchMode}, {"DrawPrimitive", 3, DrawPrimitive},
       {"DrawIndexedPrimitive", 6, DrawIndexedPrimitive}, {"DrawPrimitiveUP", 4, DrawPrimitiveUP},
       {"DrawIndexedPrimitiveUP", 8, DrawIndexedPrimitiveUP}, {"ProcessVertices", 6, nullptr},
       {"CreateVertexDeclaration", 2, CreateVertexDeclaration}, {"SetVertexDeclaration", 1, SetVertexDeclaration},
       {"GetVertexDeclaration", 1, GetVertexDeclaration}, {"SetFVF", 1, SetFVF}, {"GetFVF", 1, GetFVF},
       {"CreateVertexShader", 2, CreateVertexShader}, {"SetVertexShader", 1, SetVertexShader},
       {"GetVertexShader", 1, GetVertexShader}, {"SetVertexShaderConstantF", 3, SetVertexShaderConstantF},
       {"GetVertexShaderConstantF", 3, GetVertexShaderConstantF}, {"SetVertexShaderConstantI", 3, SetVertexShaderConstantI},
       {"GetVertexShaderConstantI", 3, nullptr}, {"SetVertexShaderConstantB", 3, SetVertexShaderConstantB},
       {"GetVertexShaderConstantB", 3, nullptr}, {"SetStreamSource", 4, SetStreamSource},
       {"GetStreamSource", 4, GetStreamSource}, {"SetStreamSourceFreq", 2, SetStreamSourceFreq},
       {"GetStreamSourceFreq", 2, GetStreamSourceFreq}, {"SetIndices", 1, SetIndices}, {"GetIndices", 1, GetIndices},
       {"CreatePixelShader", 2, CreatePixelShader}, {"SetPixelShader", 1, SetPixelShader}, {"GetPixelShader", 1, GetPixelShader},
       {"SetPixelShaderConstantF", 3, SetPixelShaderConstantF}, {"GetPixelShaderConstantF", 3, GetPixelShaderConstantF},
       {"SetPixelShaderConstantI", 3, SetPixelShaderConstantI}, {"GetPixelShaderConstantI", 3, nullptr},
       {"SetPixelShaderConstantB", 3, SetPixelShaderConstantB}, {"GetPixelShaderConstantB", 3, nullptr},
       {"DrawRectPatch", 3, nullptr}, {"DrawTriPatch", 3, nullptr}, {"DeletePatch", 1, nullptr},
       {"CreateQuery", 2, CreateQuery}});
  return serialized(i);
}

}  // namespace d3d
