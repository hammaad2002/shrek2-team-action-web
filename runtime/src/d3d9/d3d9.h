// Direct3D 9 HLE: object model and device state. Rendering is delegated to gl::Renderer.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../hle/com.h"

namespace d3d {

using rt::ComObject;

// ---- Constants ---------------------------------------------------------------------------------

constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return (uint32_t)(uint8_t)a | (uint32_t)(uint8_t)b << 8 | (uint32_t)(uint8_t)c << 16 | (uint32_t)(uint8_t)d << 24;
}

enum Format : uint32_t {
  FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
  FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_A8 = 28, FMT_X4R4G4B4 = 30, FMT_A8B8G8R8 = 32, FMT_X8B8G8R8 = 33,
  FMT_A8P8 = 40, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51, FMT_A4L4 = 52, FMT_V8U8 = 60, FMT_D16_LOCKABLE = 70,
  FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79, FMT_D16 = 80, FMT_L16 = 81,
  FMT_INDEX16 = 101, FMT_INDEX32 = 102, FMT_R16F = 111, FMT_A16B16G16R16F = 113, FMT_R32F = 114,
  FMT_A32B32G32R32F = 116,
  FMT_DXT1 = fourcc('D', 'X', 'T', '1'), FMT_DXT2 = fourcc('D', 'X', 'T', '2'), FMT_DXT3 = fourcc('D', 'X', 'T', '3'),
  FMT_DXT4 = fourcc('D', 'X', 'T', '4'), FMT_DXT5 = fourcc('D', 'X', 'T', '5'),
};

struct FormatInfo {
  uint32_t bpp;        // bits per pixel (uncompressed)
  uint32_t block;      // bytes per 4x4 block (compressed), else 0
  bool depth;
  bool known;
};
FormatInfo format_info(uint32_t fmt);
const char* format_name(uint32_t fmt);
uint32_t row_pitch(uint32_t fmt, uint32_t width);
uint32_t level_rows(uint32_t fmt, uint32_t height);  // number of pitch rows (block rows for DXT)

enum Pool : uint32_t { POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2, POOL_SCRATCH = 3 };
enum Usage : uint32_t { USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2, USAGE_WRITEONLY = 8, USAGE_DYNAMIC = 0x200, USAGE_AUTOGENMIPMAP = 0x400 };
enum ResType : uint32_t { RTYPE_SURFACE = 1, RTYPE_VOLUME = 2, RTYPE_TEXTURE = 3, RTYPE_VOLUMETEXTURE = 4, RTYPE_CUBETEXTURE = 5, RTYPE_VERTEXBUFFER = 6, RTYPE_INDEXBUFFER = 7 };

constexpr uint32_t D3DERR_INVALIDCALL = 0x8876086c, D3DERR_NOTAVAILABLE = 0x8876086a, D3DERR_DEVICELOST = 0x88760868,
                   D3DERR_OUTOFVIDEOMEMORY = 0x8876017c, D3DERR_NOTFOUND = 0x88760866;

struct Mat4 {
  float m[16];  // row-major as in D3DMATRIX (_11.._44)
  static Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }
};

// ---- Objects ------------------------------------------------------------------------------------

class Device;

class Resource : public ComObject {
 public:
  Device* device = nullptr;
  uint32_t priority = 0;
  virtual uint32_t rtype() const = 0;
};

class Surface;

// A texture level / render target / depth buffer / offscreen surface.
class Surface : public Resource {
 public:
  const char* type_name() const override { return "Surface"; }
  uint32_t rtype() const override { return RTYPE_SURFACE; }
  uint32_t width = 0, height = 0, format = 0, usage = 0, pool = 0;
  uint32_t mem = 0;      // guest shadow memory (0 until needed)
  uint32_t pitch = 0;
  bool dirty = false;    // shadow memory changed since the last upload
  bool locked = false;
  // Container (texture) and position within it; null for standalone surfaces.
  class BaseTexture* container = nullptr;
  uint32_t level = 0, face = 0;
  uint32_t gl_rb = 0;   // standalone render target / depth renderbuffer (gl::)
  uint32_t gl_tex = 0;  // standalone colour render target backed by a texture
  uint32_t guest_ptr = 0;
  uint32_t ensure_memory();
  ~Surface() override;
};

class BaseTexture : public Resource {
 public:
  uint32_t width = 0, height = 0, levels = 0, format = 0, usage = 0, pool = 0;
  uint32_t lod = 0, autogen_filter = 2;
  std::vector<Surface*> surfaces;  // level-major (faces interleaved for cubes: level * faces + face)
  uint32_t faces = 1;
  uint32_t gl_tex = 0;
  bool any_dirty = true;
  ~BaseTexture() override;
};

class Texture : public BaseTexture {
 public:
  const char* type_name() const override { return "Texture"; }
  uint32_t rtype() const override { return RTYPE_TEXTURE; }
};

class CubeTexture : public BaseTexture {
 public:
  const char* type_name() const override { return "CubeTexture"; }
  uint32_t rtype() const override { return RTYPE_CUBETEXTURE; }
};

class Buffer : public Resource {
 public:
  uint32_t length = 0, usage = 0, fvf_or_format = 0, pool = 0;
  uint32_t mem = 0;  // guest memory
  uint32_t gl_buf = 0;
  uint32_t dirty_lo = 0, dirty_hi = 0;  // byte range written since the last upload
  void mark_dirty(uint32_t off, uint32_t size);
  ~Buffer() override;
};

class VertexBuffer : public Buffer {
 public:
  const char* type_name() const override { return "VertexBuffer"; }
  uint32_t rtype() const override { return RTYPE_VERTEXBUFFER; }
};

class IndexBuffer : public Buffer {
 public:
  const char* type_name() const override { return "IndexBuffer"; }
  uint32_t rtype() const override { return RTYPE_INDEXBUFFER; }
};

struct VertexElement {
  uint16_t stream, offset;
  uint8_t type, method, usage, usage_index;
};

class VertexDeclaration : public ComObject {
 public:
  const char* type_name() const override { return "VertexDeclaration"; }
  Device* device = nullptr;
  std::vector<VertexElement> elements;  // without the END marker
  uint32_t fvf = 0;                     // when created from an FVF
};

class Shader : public ComObject {
 public:
  Device* device = nullptr;
  bool pixel = false;
  std::vector<uint32_t> tokens;
  uint32_t program_id = 0;  // translated GLSL (renderer cache key)
  const char* type_name() const override { return pixel ? "PixelShader" : "VertexShader"; }
  ~Shader() override;
};

class StateBlock;
class Query : public ComObject {
 public:
  const char* type_name() const override { return "Query"; }
  Device* device = nullptr;
  uint32_t type = 0;
};

class SwapChain : public ComObject {
 public:
  const char* type_name() const override { return "SwapChain"; }
  Device* device = nullptr;
};

// ---- Device state ------------------------------------------------------------------------------

struct Light {
  uint32_t type = 3;
  float diffuse[4]{1, 1, 1, 0}, specular[4]{}, ambient[4]{}, position[3]{}, direction[3]{0, 0, 1};
  float range = 0, falloff = 0, att0 = 0, att1 = 0, att2 = 0, theta = 0, phi = 0;
};

struct Material {
  float diffuse[4]{1, 1, 1, 1}, ambient[4]{}, specular[4]{}, emissive[4]{}, power = 0;
};

struct Viewport {
  uint32_t x = 0, y = 0, w = 640, h = 480;
  float min_z = 0, max_z = 1;
};

struct Stream {
  VertexBuffer* vb = nullptr;
  uint32_t offset = 0, stride = 0;
};

struct State {
  std::array<uint32_t, 256> rs{};
  std::array<std::array<uint32_t, 33>, 8> tss{};
  std::array<std::array<uint32_t, 14>, 20> ss{};  // samplers 0-15 + D3DVERTEXTEXTURESAMPLER0-3
  std::map<uint32_t, Mat4> transforms;
  Viewport viewport;
  Material material;
  std::map<uint32_t, Light> lights;
  std::map<uint32_t, bool> light_enable;
  std::array<std::array<float, 4>, 6> clip_planes{};
  std::array<BaseTexture*, 20> textures{};
  std::array<Stream, 16> streams{};
  std::array<uint32_t, 16> stream_freq{};
  IndexBuffer* indices = nullptr;
  VertexDeclaration* decl = nullptr;
  uint32_t fvf = 0;
  Shader* vs = nullptr;
  Shader* ps = nullptr;
  std::array<float, 256 * 4> vs_f{};
  std::array<int32_t, 16 * 4> vs_i{};
  std::array<uint32_t, 16> vs_b{};
  std::array<float, 224 * 4> ps_f{};
  std::array<int32_t, 16 * 4> ps_i{};
  std::array<uint32_t, 16> ps_b{};
  struct {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
  } scissor;
};

void init_default_state(State& s);

class Device : public ComObject {
 public:
  const char* type_name() const override { return "Direct3DDevice9"; }
  uint32_t guest_ptr = 0;
  uint32_t d3d_ptr = 0;    // the IDirect3D9 that created us
  uint32_t hwnd = 0;
  uint32_t behavior = 0;
  uint32_t pp[14] = {};    // D3DPRESENT_PARAMETERS
  State st;
  std::array<Surface*, 4> render_targets{};
  Surface* depth_stencil = nullptr;
  Surface* back_buffer = nullptr;
  Surface* auto_depth = nullptr;
  StateBlock* recording = nullptr;
  bool in_scene = false;
  uint64_t frame = 0;
  uint32_t gamma[3 * 256] = {};
  // Statistics for the current frame.
  uint32_t draws = 0, prims = 0;
};

class StateBlock : public ComObject {
 public:
  const char* type_name() const override { return "StateBlock"; }
  Device* device = nullptr;
  State snapshot;
  std::vector<uint32_t> ops;  // reserved for recorded blocks
  bool recorded = false;      // from Begin/EndStateBlock (applies the whole snapshot either way)
  ~StateBlock() override;
};

// D3D reference counting: texture levels share their container's count; zero deletes.
void add_ref(ComObject* o);
void release_ref(ComObject* o);

// All Direct3D calls are serialised: the game may touch resources from its loader thread.
std::recursive_mutex& api_lock();
inline rt::ComInterface& serialized(rt::ComInterface& i) {
  i.set_serialize(&api_lock());
  return i;
}

// Interfaces (vtables), shared between translation units.
rt::ComInterface& iface_device();
rt::ComInterface& iface_texture();
rt::ComInterface& iface_cube_texture();
rt::ComInterface& iface_surface();
rt::ComInterface& iface_vertex_buffer();
rt::ComInterface& iface_index_buffer();
rt::ComInterface& iface_vertex_decl();
rt::ComInterface& iface_vertex_shader();
rt::ComInterface& iface_pixel_shader();
rt::ComInterface& iface_state_block();
rt::ComInterface& iface_query();
rt::ComInterface& iface_swap_chain();

// Wrap a host object for the guest, remembering its guest pointer.
uint32_t expose(ComObject* obj, rt::ComInterface& iface);
uint32_t guest_ptr_of(ComObject* obj);  // primary guest pointer (first wrapper)

Surface* create_surface(Device* dev, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool);
// Queue an upload of a surface's shadow memory (texture level or standalone surface).
void upload_surface(Surface* s);
void state_block_apply(StateBlock* sb);
void state_block_capture(StateBlock* sb);
void state_block_forget(StateBlock* sb);
std::vector<VertexElement> fvf_elements(uint32_t fvf);
uint32_t fvf_stride(uint32_t fvf);
BaseTexture* create_texture(Device* dev, bool cube, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool);

}  // namespace d3d
