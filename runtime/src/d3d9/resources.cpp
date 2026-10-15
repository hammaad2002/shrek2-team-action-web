// Direct3D 9 resources: textures, surfaces, vertex/index buffers, declarations, shaders, state
// blocks, queries and swap chains. Lockable data lives in guest memory; unlocking queues an upload.
#include "../core/objects.h"
#include "recorder.h"

using namespace rt;

namespace d3d {

namespace {
uint32_t ok(Cpu*, ComObject*) { return S_OK; }
uint32_t zero(Cpu*, ComObject*) { return 0; }
template <typename T>
T* as(ComObject* o) {
  return static_cast<T*>(o);
}
}  // namespace

uint32_t Surface::ensure_memory() {
  if (!mem) {
    pitch = row_pitch(format, width);
    mem = heap_alloc(process_heap(), pitch * level_rows(format, height), true);
  }
  return mem;
}

namespace {
struct LockInfo {
  uint32_t off, size;
};
std::map<Buffer*, LockInfo> g_locks;
}  // namespace

Surface::~Surface() {
  if (mem) heap_free(process_heap(), mem);
  if (!container) {
    if (gl_tex) rec().op(cmd::OpDestroyTexture, {gl_tex});
    if (gl_rb) rec().op(cmd::OpDestroyRenderbuffer, {gl_rb});
  }
}

BaseTexture::~BaseTexture() {
  if (gl_tex) rec().op(cmd::OpDestroyTexture, {gl_tex});
  for (Surface* s : surfaces) delete s;
}

Buffer::~Buffer() {
  g_locks.erase(this);
  if (gl_buf) rec().op(cmd::OpDestroyBuffer, {gl_buf});
  if (mem) heap_free(process_heap(), mem);
}

Shader::~Shader() { rec().op(cmd::OpDestroyShader, {program_id}); }

StateBlock::~StateBlock() { state_block_forget(this); }

static ComObject* ref_owner(ComObject* o) {
  if (auto* s = dynamic_cast<Surface*>(o); s && s->container) return s->container;
  return o;
}

void add_ref(ComObject* o) {
  if (o) ref_owner(o)->refs++;
}

void release_ref(ComObject* o) {
  if (!o) return;
  o = ref_owner(o);
  if (--o->refs == 0) delete o;
}

void Buffer::mark_dirty(uint32_t off, uint32_t size) {
  if (dirty_hi <= dirty_lo) dirty_lo = off, dirty_hi = off + size;
  else dirty_lo = std::min(dirty_lo, off), dirty_hi = std::max(dirty_hi, off + size);
}

SurfaceRef surface_ref(Surface* s) {
  if (!s) return {0, 0, 0, 0};
  if (s->device && s == s->device->back_buffer) return {1, 0, 0, 0};
  if (s->container) return {2, s->container->gl_tex, s->face, s->level};
  if (s->gl_tex) return {2, s->gl_tex, 0, 0};
  if (s->gl_rb) return {3, s->gl_rb, 0, 0};
  return {0, 0, 0, 0};
}

void upload_surface(Surface* s) {
  if (!s->mem) return;
  uint32_t tex = s->container ? s->container->gl_tex : s->gl_tex;
  if (!tex) {
    // Standalone shadow-memory surface: give it a texture so it can be stretched/drawn.
    s->gl_tex = tex = new_resource_id();
    rec().op(cmd::OpCreateTexture, {tex, 0, s->width, s->height, 1, s->format, 0});
  }
  uint32_t rows = level_rows(s->format, s->height);
  uint32_t bytes = s->pitch * rows;
  rec().op(cmd::OpUploadTexture, {tex, s->face, s->level, s->width, s->height, s->format, s->pitch, rows, bytes});
  rec().bytes(host(s->mem), bytes);
  s->dirty = false;
}

Surface* create_surface(Device* dev, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool) {
  auto* s = new Surface();
  s->device = dev;
  s->width = w;
  s->height = h;
  s->format = fmt;
  s->usage = usage;
  s->pool = pool;
  s->pitch = row_pitch(fmt, w);
  if (usage & USAGE_DEPTHSTENCIL) {
    s->gl_rb = new_resource_id();
    rec().op(cmd::OpCreateRenderbuffer, {s->gl_rb, w, h, fmt});
  } else if (usage & USAGE_RENDERTARGET) {
    s->gl_tex = new_resource_id();
    rec().op(cmd::OpCreateTexture, {s->gl_tex, 0, w, h, 1, fmt, 1});
  }
  return s;
}

BaseTexture* create_texture(Device* dev, bool cube, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
  BaseTexture* t = cube ? (BaseTexture*)new CubeTexture() : (BaseTexture*)new Texture();
  t->device = dev;
  t->width = w;
  t->height = h;
  t->format = fmt;
  t->usage = usage;
  t->pool = pool;
  t->faces = cube ? 6 : 1;
  uint32_t full = 1;
  for (uint32_t m = std::max(w, h); m > 1; m >>= 1) full++;
  if (usage & USAGE_AUTOGENMIPMAP) levels = 1;
  t->levels = levels == 0 || levels > full ? full : levels;
  t->gl_tex = new_resource_id();
  rec().op(cmd::OpCreateTexture, {t->gl_tex, cube ? 1u : 0u, w, h, t->levels, fmt, (usage & USAGE_RENDERTARGET) ? 1u : 0u});
  for (uint32_t l = 0; l < t->levels; l++) {
    for (uint32_t f = 0; f < t->faces; f++) {
      auto* s = new Surface();
      s->device = dev;
      s->width = std::max(1u, w >> l);
      s->height = std::max(1u, h >> l);
      s->format = fmt;
      s->usage = usage;
      s->pool = pool;
      s->pitch = row_pitch(fmt, s->width);
      s->container = t;
      s->level = l;
      s->face = f;
      t->surfaces.push_back(s);
    }
  }
  return t;
}

namespace {

// ---- IDirect3DResource9 helpers -------------------------------------------------------------

uint32_t GetDevice(Cpu* c, ComObject* self) {
  Device* d = nullptr;
  if (auto* r = dynamic_cast<Resource*>(self)) d = r->device;
  else if (auto* v = dynamic_cast<VertexDeclaration*>(self)) d = v->device;
  else if (auto* s = dynamic_cast<Shader*>(self)) d = s->device;
  else if (auto* b = dynamic_cast<StateBlock*>(self)) d = b->device;
  else if (auto* q = dynamic_cast<Query*>(self)) d = q->device;
  else if (auto* sc = dynamic_cast<SwapChain*>(self)) d = sc->device;
  if (!d) return D3DERR_INVALIDCALL;
  d->refs++;
  st32(arg(c, 1), d->guest_ptr);
  return S_OK;
}

uint32_t SetPriority(Cpu* c, ComObject* self) {
  auto* r = as<Resource>(self);
  uint32_t old = r->priority;
  r->priority = arg(c, 1);
  return old;
}
uint32_t GetPriority(Cpu*, ComObject* self) { return as<Resource>(self)->priority; }
uint32_t GetType(Cpu*, ComObject* self) { return as<Resource>(self)->rtype(); }

// ---- Textures --------------------------------------------------------------------------------

uint32_t surface_guest(Surface* s) {
  if (s->wrappers.empty()) com_wrap(s, iface_surface());
  return s->wrappers[0];
}

uint32_t tex_GetLevelCount(Cpu*, ComObject* self) { return as<BaseTexture>(self)->levels; }
uint32_t tex_SetLOD(Cpu* c, ComObject* self) {
  uint32_t old = as<BaseTexture>(self)->lod;
  as<BaseTexture>(self)->lod = arg(c, 1);
  return old;
}
uint32_t tex_GetLOD(Cpu*, ComObject* self) { return as<BaseTexture>(self)->lod; }
uint32_t tex_SetAutoGenFilterType(Cpu* c, ComObject* self) {
  as<BaseTexture>(self)->autogen_filter = arg(c, 1);
  return S_OK;
}
uint32_t tex_GetAutoGenFilterType(Cpu*, ComObject* self) { return as<BaseTexture>(self)->autogen_filter; }

void write_desc(uint32_t p, Surface* s) {
  st32(p + 0, s->format);
  st32(p + 4, RTYPE_SURFACE);
  st32(p + 8, s->usage);
  st32(p + 12, s->pool);
  st32(p + 16, 0);
  st32(p + 20, 0);
  st32(p + 24, s->width);
  st32(p + 28, s->height);
}

Surface* level_surface(BaseTexture* t, uint32_t face, uint32_t level) {
  uint32_t i = level * t->faces + face;
  return i < t->surfaces.size() ? t->surfaces[i] : nullptr;
}

uint32_t tex_GetLevelDesc(Cpu* c, ComObject* self) {
  Surface* s = level_surface(as<BaseTexture>(self), 0, arg(c, 1));
  if (!s) return D3DERR_INVALIDCALL;
  write_desc(arg(c, 2), s);
  return S_OK;
}

uint32_t tex_GetSurfaceLevel(Cpu* c, ComObject* self) {
  Surface* s = level_surface(as<BaseTexture>(self), 0, arg(c, 1));
  if (!s) return D3DERR_INVALIDCALL;
  self->refs++;  // surfaces share the container's reference count
  st32(arg(c, 2), surface_guest(s));
  return S_OK;
}

uint32_t lock_surface(Surface* s, uint32_t locked_rect, uint32_t rect, uint32_t flags) {
  uint32_t mem = s->ensure_memory();
  if ((s->usage & USAGE_RENDERTARGET) && !(flags & 0x2000)) {  // not D3DLOCK_DISCARD: read the GPU contents
    SurfaceRef r = surface_ref(s);
    rec().op(cmd::OpReadback, {r.kind, r.id, r.face, r.level, s->width, s->height, s->format, mem, s->pitch});
    flush_and_wait();
  }
  uint32_t off = 0;
  if (rect) {
    uint32_t left = ld32(rect), top = ld32(rect + 4);
    FormatInfo f = format_info(s->format);
    off = f.block ? (top / 4) * s->pitch + (left / 4) * f.block : top * s->pitch + left * f.bpp / 8;
  }
  st32(locked_rect, s->pitch);
  st32(locked_rect + 4, mem + off);
  s->locked = true;
  if (!(flags & 0x10)) s->dirty = true;  // D3DLOCK_READONLY
  return S_OK;
}

uint32_t unlock_surface(Surface* s) {
  s->locked = false;
  if (s->dirty && !(s->usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL))) upload_surface(s);
  return S_OK;
}

uint32_t tex_LockRect(Cpu* c, ComObject* self) {
  Surface* s = level_surface(as<BaseTexture>(self), 0, arg(c, 1));
  if (!s) return D3DERR_INVALIDCALL;
  return lock_surface(s, arg(c, 2), arg(c, 3), arg(c, 4));
}

uint32_t tex_UnlockRect(Cpu* c, ComObject* self) {
  Surface* s = level_surface(as<BaseTexture>(self), 0, arg(c, 1));
  return s ? unlock_surface(s) : D3DERR_INVALIDCALL;
}

uint32_t tex_GenerateMipSubLevels(Cpu*, ComObject*) { return 0; }

uint32_t cube_GetLevelDesc(Cpu* c, ComObject* self) { return tex_GetLevelDesc(c, self); }
uint32_t cube_GetCubeMapSurface(Cpu* c, ComObject* self) {
  Surface* s = level_surface(as<BaseTexture>(self), arg(c, 1), arg(c, 2));
  if (!s) return D3DERR_INVALIDCALL;
  self->refs++;
  st32(arg(c, 3), surface_guest(s));
  return S_OK;
}
uint32_t cube_LockRect(Cpu* c, ComObject* self) {
  Surface* s = level_surface(as<BaseTexture>(self), arg(c, 1), arg(c, 2));
  if (!s) return D3DERR_INVALIDCALL;
  return lock_surface(s, arg(c, 3), arg(c, 4), arg(c, 5));
}
uint32_t cube_UnlockRect(Cpu* c, ComObject* self) {
  Surface* s = level_surface(as<BaseTexture>(self), arg(c, 1), arg(c, 2));
  return s ? unlock_surface(s) : D3DERR_INVALIDCALL;
}

std::vector<rt::ComMethod> resource_methods(std::vector<rt::ComMethod> rest, rt::ComFn release = com_Release) {
  std::vector<rt::ComMethod> m = {{"QueryInterface", 2, com_QueryInterface_none}, {"AddRef", 0, com_AddRef},
                                  {"Release", 0, release},       {"GetDevice", 1, GetDevice},
                                  {"SetPrivateData", 4, ok},     {"GetPrivateData", 3, nullptr},
                                  {"FreePrivateData", 1, ok},    {"SetPriority", 1, SetPriority},
                                  {"GetPriority", 0, GetPriority}, {"PreLoad", 0, zero},
                                  {"GetType", 0, GetType}};
  m.insert(m.end(), rest.begin(), rest.end());
  return m;
}

std::vector<rt::ComMethod> base_texture_methods(std::vector<rt::ComMethod> rest) {
  std::vector<rt::ComMethod> m = {{"SetLOD", 1, tex_SetLOD},
                                  {"GetLOD", 0, tex_GetLOD},
                                  {"GetLevelCount", 0, tex_GetLevelCount},
                                  {"SetAutoGenFilterType", 1, tex_SetAutoGenFilterType},
                                  {"GetAutoGenFilterType", 0, tex_GetAutoGenFilterType},
                                  {"GenerateMipSubLevels", 0, tex_GenerateMipSubLevels}};
  m.insert(m.end(), rest.begin(), rest.end());
  return resource_methods(m);
}

// ---- Surfaces --------------------------------------------------------------------------------

uint32_t surf_AddRef(Cpu* c, ComObject* self) {
  auto* s = as<Surface>(self);
  if (s->container) return (uint32_t)++s->container->refs;
  return com_AddRef(c, self);
}

uint32_t surf_Release(Cpu* c, ComObject* self) {
  auto* s = as<Surface>(self);
  if (s->container) {
    int32_t r = --s->container->refs;
    if (r == 0) delete s->container;  // also deletes its surfaces
    return (uint32_t)std::max(r, 0);
  }
  return com_Release(c, self);
}

uint32_t surf_GetContainer(Cpu* c, ComObject* self) {
  auto* s = as<Surface>(self);
  if (!s->container) return D3DERR_INVALIDCALL;
  s->container->refs++;
  st32(arg(c, 2), guest_ptr_of(s->container));
  return S_OK;
}

uint32_t surf_GetDesc(Cpu* c, ComObject* self) {
  write_desc(arg(c, 1), as<Surface>(self));
  return S_OK;
}

uint32_t surf_LockRect(Cpu* c, ComObject* self) { return lock_surface(as<Surface>(self), arg(c, 1), arg(c, 2), arg(c, 3)); }
uint32_t surf_UnlockRect(Cpu*, ComObject* self) { return unlock_surface(as<Surface>(self)); }

// ---- Buffers ---------------------------------------------------------------------------------

uint32_t buf_Lock(Cpu* c, ComObject* self) {
  auto* b = as<Buffer>(self);
  uint32_t off = arg(c, 1), size = arg(c, 2), out = arg(c, 3), flags = arg(c, 4);
  if (size == 0 && off == 0) size = b->length;
  if (off > b->length) return D3DERR_INVALIDCALL;
  size = std::min(size, b->length - off);
  st32(out, b->mem + off);
  if (!(flags & 0x10)) g_locks[b] = {off, size};  // not D3DLOCK_READONLY
  return S_OK;
}

uint32_t buf_Unlock(Cpu*, ComObject* self) {
  auto* b = as<Buffer>(self);
  auto it = g_locks.find(b);
  if (it == g_locks.end()) return S_OK;
  LockInfo l = it->second;
  g_locks.erase(it);
  if (l.size) {
    rec().op(cmd::OpUploadBuffer, {b->gl_buf, l.off, l.size});
    rec().bytes(host(b->mem + l.off), l.size);
  }
  return S_OK;
}

uint32_t buf_GetDesc(Cpu* c, ComObject* self) {
  auto* b = as<Buffer>(self);
  uint32_t p = arg(c, 1);
  bool vb = dynamic_cast<VertexBuffer*>(b) != nullptr;
  st32(p + 0, vb ? 100u : b->fvf_or_format);  // Format (D3DFMT_VERTEXDATA for VBs)
  st32(p + 4, vb ? RTYPE_VERTEXBUFFER : RTYPE_INDEXBUFFER);
  st32(p + 8, b->usage);
  st32(p + 12, b->pool);
  st32(p + 16, b->length);
  if (vb) st32(p + 20, b->fvf_or_format);
  return S_OK;
}

uint32_t buf_Release(Cpu* c, ComObject* self) { return com_Release(c, self); }

// ---- Declarations, shaders, state blocks, queries, swap chains ----------------------------

uint32_t decl_GetDeclaration(Cpu* c, ComObject* self) {
  auto* d = as<VertexDeclaration>(self);
  uint32_t out = arg(c, 1), n = arg(c, 2);
  uint32_t count = (uint32_t)d->elements.size() + 1;
  if (out) {
    for (uint32_t i = 0; i < d->elements.size(); i++) {
      const VertexElement& e = d->elements[i];
      st16(out + 8 * i, e.stream);
      st16(out + 8 * i + 2, e.offset);
      st8(out + 8 * i + 4, e.type);
      st8(out + 8 * i + 5, e.method);
      st8(out + 8 * i + 6, e.usage);
      st8(out + 8 * i + 7, e.usage_index);
    }
    uint32_t end = out + 8 * (count - 1);
    st16(end, 0xff);
    st16(end + 2, 0);
    st8(end + 4, 17);
    st8(end + 5, 0);
    st8(end + 6, 0);
    st8(end + 7, 0);
  }
  if (n) st32(n, count);
  return S_OK;
}

uint32_t shader_GetFunction(Cpu* c, ComObject* self) {
  auto* s = as<Shader>(self);
  uint32_t out = arg(c, 1), size = arg(c, 2);
  uint32_t bytes = (uint32_t)s->tokens.size() * 4;
  if (out) memcpy(host(out), s->tokens.data(), std::min(bytes, ld32(size)));
  st32(size, bytes);
  return S_OK;
}

uint32_t shader_Release(Cpu* c, ComObject* self) { return com_Release(c, self); }

uint32_t sb_Capture(Cpu*, ComObject* self) {
  state_block_capture(as<StateBlock>(self));
  return S_OK;
}
uint32_t sb_Apply(Cpu*, ComObject* self) {
  state_block_apply(as<StateBlock>(self));
  return S_OK;
}
uint32_t sb_Release(Cpu* c, ComObject* self) { return com_Release(c, self); }

uint32_t q_GetType(Cpu*, ComObject* self) { return as<Query>(self)->type; }
uint32_t q_GetDataSize(Cpu*, ComObject* self) { return as<Query>(self)->type == 9 ? 4 : 4; }
uint32_t q_GetData(Cpu* c, ComObject* self) {
  uint32_t data = arg(c, 1), size = arg(c, 2);
  if (data && size >= 4) st32(data, as<Query>(self)->type == 9 ? 100000u : 1u);  // occlusion: "visible"
  return S_OK;
}

uint32_t sc_Present(Cpu* c, ComObject* self) {
  (void)c;
  Device* d = as<SwapChain>(self)->device;
  rec().op(cmd::OpPresent, {(uint32_t)d->frame});
  d->frame++;
  flush();
  return S_OK;
}

uint32_t sc_GetBackBuffer(Cpu* c, ComObject* self) {
  Device* d = as<SwapChain>(self)->device;
  add_ref(d->back_buffer);
  st32(arg(c, 3), guest_ptr_of(d->back_buffer));
  return S_OK;
}

uint32_t sc_GetPresentParameters(Cpu* c, ComObject* self) {
  Device* d = as<SwapChain>(self)->device;
  for (int i = 0; i < 14; i++) st32(arg(c, 1) + 4 * (uint32_t)i, d->pp[i]);
  return S_OK;
}

}  // namespace

rt::ComInterface& iface_texture() {
  static rt::ComInterface i("IDirect3DTexture9", base_texture_methods({{"GetLevelDesc", 2, tex_GetLevelDesc},
                                                                       {"GetSurfaceLevel", 2, tex_GetSurfaceLevel},
                                                                       {"LockRect", 4, tex_LockRect},
                                                                       {"UnlockRect", 1, tex_UnlockRect},
                                                                       {"AddDirtyRect", 1, ok}}));
  return serialized(i);
}

rt::ComInterface& iface_cube_texture() {
  static rt::ComInterface i("IDirect3DCubeTexture9", base_texture_methods({{"GetLevelDesc", 2, cube_GetLevelDesc},
                                                                           {"GetCubeMapSurface", 3, cube_GetCubeMapSurface},
                                                                           {"LockRect", 5, cube_LockRect},
                                                                           {"UnlockRect", 2, cube_UnlockRect},
                                                                           {"AddDirtyRect", 2, ok}}));
  return serialized(i);
}

rt::ComInterface& iface_surface() {
  static rt::ComInterface i("IDirect3DSurface9", [] {
    auto m = resource_methods({{"GetContainer", 2, surf_GetContainer},
                               {"GetDesc", 1, surf_GetDesc},
                               {"LockRect", 3, surf_LockRect},
                               {"UnlockRect", 0, surf_UnlockRect},
                               {"GetDC", 1, nullptr},
                               {"ReleaseDC", 1, nullptr}},
                              surf_Release);
    m[1].fn = surf_AddRef;
    return m;
  }());
  return serialized(i);
}

rt::ComInterface& iface_vertex_buffer() {
  static rt::ComInterface i("IDirect3DVertexBuffer9", resource_methods({{"Lock", 4, buf_Lock}, {"Unlock", 0, buf_Unlock}, {"GetDesc", 1, buf_GetDesc}}, buf_Release));
  return serialized(i);
}

rt::ComInterface& iface_index_buffer() {
  static rt::ComInterface i("IDirect3DIndexBuffer9", resource_methods({{"Lock", 4, buf_Lock}, {"Unlock", 0, buf_Unlock}, {"GetDesc", 1, buf_GetDesc}}, buf_Release));
  return serialized(i);
}

rt::ComInterface& iface_vertex_decl() {
  static rt::ComInterface i("IDirect3DVertexDeclaration9", {{"QueryInterface", 2, com_QueryInterface_none},
                                                            {"AddRef", 0, com_AddRef},
                                                            {"Release", 0, com_Release},
                                                            {"GetDevice", 1, GetDevice},
                                                            {"GetDeclaration", 2, decl_GetDeclaration}});
  return serialized(i);
}

rt::ComInterface& iface_vertex_shader() {
  static rt::ComInterface i("IDirect3DVertexShader9", {{"QueryInterface", 2, com_QueryInterface_none},
                                                       {"AddRef", 0, com_AddRef},
                                                       {"Release", 0, shader_Release},
                                                       {"GetDevice", 1, GetDevice},
                                                       {"GetFunction", 2, shader_GetFunction}});
  return serialized(i);
}

rt::ComInterface& iface_pixel_shader() {
  static rt::ComInterface i("IDirect3DPixelShader9", {{"QueryInterface", 2, com_QueryInterface_none},
                                                      {"AddRef", 0, com_AddRef},
                                                      {"Release", 0, shader_Release},
                                                      {"GetDevice", 1, GetDevice},
                                                      {"GetFunction", 2, shader_GetFunction}});
  return serialized(i);
}

rt::ComInterface& iface_state_block() {
  static rt::ComInterface i("IDirect3DStateBlock9", {{"QueryInterface", 2, com_QueryInterface_none},
                                                     {"AddRef", 0, com_AddRef},
                                                     {"Release", 0, sb_Release},
                                                     {"GetDevice", 1, GetDevice},
                                                     {"Capture", 0, sb_Capture},
                                                     {"Apply", 0, sb_Apply}});
  return serialized(i);
}

rt::ComInterface& iface_query() {
  static rt::ComInterface i("IDirect3DQuery9", {{"QueryInterface", 2, com_QueryInterface_none},
                                                {"AddRef", 0, com_AddRef},
                                                {"Release", 0, com_Release},
                                                {"GetDevice", 1, GetDevice},
                                                {"GetType", 0, q_GetType},
                                                {"GetDataSize", 0, q_GetDataSize},
                                                {"Issue", 1, ok},
                                                {"GetData", 3, q_GetData}});
  return serialized(i);
}

rt::ComInterface& iface_swap_chain() {
  static rt::ComInterface i("IDirect3DSwapChain9", {{"QueryInterface", 2, com_QueryInterface_none},
                                                    {"AddRef", 0, com_AddRef},
                                                    {"Release", 0, com_Release},
                                                    {"Present", 5, sc_Present},
                                                    {"GetFrontBufferData", 1, nullptr},
                                                    {"GetBackBuffer", 3, sc_GetBackBuffer},
                                                    {"GetRasterStatus", 1, nullptr},
                                                    {"GetDisplayMode", 1, nullptr},
                                                    {"GetDevice", 1, GetDevice},
                                                    {"GetPresentParameters", 1, sc_GetPresentParameters}});
  return serialized(i);
}

}  // namespace d3d
