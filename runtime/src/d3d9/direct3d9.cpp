// IDirect3D9: adapter enumeration, capabilities and device creation.
#include "../core/objects.h"
#include "../platform/platform.h"
#include "d3d9.h"
#include "recorder.h"

using namespace rt;

namespace d3d {
namespace {

class Direct3D9 : public ComObject {
 public:
  const char* type_name() const override { return "Direct3D9"; }
};

struct Mode {
  uint32_t w, h;
};
const Mode kModes[] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960}, {1280, 1024}, {1600, 1200}, {1920, 1080}};

bool supported_texture_format(uint32_t f) {
  switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
    case FMT_A4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_DXT1: case FMT_DXT2: case FMT_DXT3:
    case FMT_DXT4: case FMT_DXT5:
      return true;
    default:
      return false;
  }
}

bool supported_depth_format(uint32_t f) { return f == FMT_D16 || f == FMT_D24S8 || f == FMT_D24X8 || f == FMT_D32 || f == FMT_D16_LOCKABLE; }

uint32_t ok(Cpu*, ComObject*) { return S_OK; }

uint32_t GetAdapterCount(Cpu*, ComObject*) { return 1; }

uint32_t GetAdapterIdentifier(Cpu* c, ComObject*) {
  uint32_t p = arg(c, 3);
  memset(host(p), 0, 1100);
  write_cstr(p, "nv4_disp.dll");
  write_cstr(p + 512, "NVIDIA GeForce FX 5900 (Shrek 2 web runtime)");
  write_cstr(p + 1024, "\\\\.\\DISPLAY1");
  st32(p + 1056, 0x000e0006);  // DriverVersion low (6.14.10.6xxx style)
  st32(p + 1060, 0x0006000e);
  st32(p + 1064, 0x10de);      // VendorId: NVIDIA
  st32(p + 1068, 0x0331);      // DeviceId
  st32(p + 1072, 0);
  st32(p + 1076, 0xa1);
  st32(p + 1096, 1);           // WHQLLevel
  return S_OK;
}

uint32_t GetAdapterModeCount(Cpu* c, ComObject*) {
  uint32_t fmt = arg(c, 2);
  return (fmt == FMT_X8R8G8B8 || fmt == FMT_R5G6B5) ? (uint32_t)(sizeof kModes / sizeof kModes[0]) : 0;
}

uint32_t EnumAdapterModes(Cpu* c, ComObject*) {
  uint32_t fmt = arg(c, 2), i = arg(c, 3), out = arg(c, 4);
  if (i >= sizeof kModes / sizeof kModes[0]) return D3DERR_INVALIDCALL;
  st32(out, kModes[i].w);
  st32(out + 4, kModes[i].h);
  st32(out + 8, 60);
  st32(out + 12, fmt);
  return S_OK;
}

uint32_t GetAdapterDisplayMode(Cpu* c, ComObject*) {
  uint32_t out = arg(c, 2);
  int w, h;
  plat::get_display_size(&w, &h);
  st32(out, (uint32_t)w);
  st32(out + 4, (uint32_t)h);
  st32(out + 8, 60);
  st32(out + 12, FMT_X8R8G8B8);
  return S_OK;
}

uint32_t CheckDeviceType(Cpu* c, ComObject*) {
  uint32_t bb = arg(c, 4);
  return (bb == FMT_X8R8G8B8 || bb == FMT_A8R8G8B8 || bb == FMT_R5G6B5 || bb == FMT_UNKNOWN) ? S_OK : D3DERR_NOTAVAILABLE;
}

uint32_t CheckDeviceFormat(Cpu* c, ComObject*) {
  uint32_t usage = arg(c, 4), rtype = arg(c, 5), fmt = arg(c, 6);
  if (usage & USAGE_DEPTHSTENCIL) return supported_depth_format(fmt) ? S_OK : D3DERR_NOTAVAILABLE;
  if (usage & USAGE_RENDERTARGET) return (fmt == FMT_A8R8G8B8 || fmt == FMT_X8R8G8B8 || fmt == FMT_R5G6B5) ? S_OK : D3DERR_NOTAVAILABLE;
  if (rtype == RTYPE_TEXTURE || rtype == RTYPE_CUBETEXTURE || rtype == RTYPE_SURFACE)
    return supported_texture_format(fmt) ? S_OK : D3DERR_NOTAVAILABLE;
  return D3DERR_NOTAVAILABLE;
}

uint32_t CheckDeviceMultiSampleType(Cpu* c, ComObject*) {
  if (arg(c, 6)) st32(arg(c, 6), 1);
  return arg(c, 5) == 0 ? S_OK : D3DERR_NOTAVAILABLE;
}

uint32_t CheckDepthStencilMatch(Cpu* c, ComObject*) { return supported_depth_format(arg(c, 5)) ? S_OK : D3DERR_NOTAVAILABLE; }

}  // namespace

void fill_caps(uint32_t p) {
  memset(host(p), 0, 304);
  st32(p + 0, 1);            // DeviceType HAL
  st32(p + 4, 0);            // AdapterOrdinal
  st32(p + 8, 0x00020000);   // Caps: D3DCAPS_READ_SCANLINE
  st32(p + 12, 0x20000000 | 0x00000020);  // Caps2: CANAUTOGENMIPMAP | FULLSCREENGAMMA
  st32(p + 16, 0x00000080);  // Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD
  st32(p + 20, 0x80000000 | 0x0000000f);  // PresentationIntervals: IMMEDIATE | ONE..FOUR
  st32(p + 24, 0x00000001);  // CursorCaps
  st32(p + 28, 0x001bbef0);  // DevCaps (HWTRANSFORMANDLIGHT, DRAWPRIMITIVES2EX, HWRASTERIZATION, PUREDEVICE...)
  st32(p + 32, 0x000f2ef2);  // PrimitiveMiscCaps (masks, cull modes, blendop, separate alpha...)
  st32(p + 36, 0x0007a091);  // RasterCaps (dither, zbias, fog, mipmaplodbias, scissortest, slopescale)
  st32(p + 40, 0x000000ff);  // ZCmpCaps: all
  st32(p + 44, 0x00003fff);  // SrcBlendCaps
  st32(p + 48, 0x00003fff);  // DestBlendCaps
  st32(p + 52, 0x000000ff);  // AlphaCmpCaps
  st32(p + 56, 0x000ffff8);  // ShadeCaps
  st32(p + 60, 0x0001ec45);  // TextureCaps: PERSPECTIVE, ALPHA, MIPMAP, CUBEMAP, MIPCUBEMAP, PROJECTED, NONPOW2CONDITIONAL
  st32(p + 64, 0x03030700);  // TextureFilterCaps
  st32(p + 68, 0x03030700);  // CubeTextureFilterCaps
  st32(p + 72, 0x03030700);  // VolumeTextureFilterCaps
  st32(p + 76, 0x0000003f);  // TextureAddressCaps
  st32(p + 80, 0x0000003f);  // VolumeTextureAddressCaps
  st32(p + 84, 0x00000003);  // LineCaps
  st32(p + 88, 4096);        // MaxTextureWidth
  st32(p + 92, 4096);        // MaxTextureHeight
  st32(p + 96, 256);         // MaxVolumeExtent
  st32(p + 100, 8192);       // MaxTextureRepeat
  st32(p + 104, 4096);       // MaxTextureAspectRatio
  st32(p + 108, 8);          // MaxAnisotropy
  st32(p + 112, f32bits(1e10f));  // MaxVertexW
  st32(p + 116, f32bits(-1e8f));  // GuardBandLeft
  st32(p + 120, f32bits(-1e8f));
  st32(p + 124, f32bits(1e8f));
  st32(p + 128, f32bits(1e8f));
  st32(p + 132, f32bits(1.0f));   // ExtentsAdjust
  st32(p + 136, 0x000001ff);      // StencilCaps
  st32(p + 140, 0x0008ffff);      // FVFCaps: 8 texcoords
  st32(p + 144, 0x03ffffff);      // TextureOpCaps: all
  st32(p + 148, 8);               // MaxTextureBlendStages
  st32(p + 152, 8);               // MaxSimultaneousTextures
  st32(p + 156, 0x0000013b);      // VertexProcessingCaps
  st32(p + 160, 8);               // MaxActiveLights
  st32(p + 164, 6);               // MaxUserClipPlanes
  st32(p + 168, 4);               // MaxVertexBlendMatrices
  st32(p + 172, 0);               // MaxVertexBlendMatrixIndex
  st32(p + 176, f32bits(64.0f));  // MaxPointSize
  st32(p + 180, 0x000fffff);      // MaxPrimitiveCount
  st32(p + 184, 0x000fffff);      // MaxVertexIndex
  st32(p + 188, 16);              // MaxStreams
  st32(p + 192, 255);             // MaxStreamStride
  st32(p + 196, 0xfffe0200);      // VertexShaderVersion vs_2_0
  st32(p + 200, 256);             // MaxVertexShaderConst
  st32(p + 204, 0xffff0200);      // PixelShaderVersion ps_2_0
  st32(p + 208, f32bits(8.0f));   // PixelShader1xMaxValue
  st32(p + 212, 0x00000001);      // DevCaps2: STREAMOFFSET
  st32(p + 216, f32bits(0.0f));   // MaxNpatchTessellationLevel
  st32(p + 228, 1);               // NumSimultaneousRTs
  st32(p + 232, 0x00000003);      // StretchRectFilterCaps (min/mag point|linear)
  st32(p + 236, 0);               // VS20Caps.Caps
  st32(p + 240, 24);              // DynamicFlowControlDepth
  st32(p + 244, 12);              // NumTemps
  st32(p + 248, 1);               // StaticFlowControlDepth
  st32(p + 252, 0);               // PS20Caps.Caps
  st32(p + 256, 0);
  st32(p + 260, 12);
  st32(p + 264, 0);
  st32(p + 268, 96);              // NumInstructionSlots
  st32(p + 284, 0x0000003f);      // DeclTypes
  st32(p + 288, 1);               // NumSimultaneousRTs (duplicate field in some headers)
  st32(p + 300, 96);              // MaxPShaderInstructionsExecuted
}

namespace {

uint32_t GetDeviceCaps(Cpu* c, ComObject*) {
  fill_caps(arg(c, 3));
  return S_OK;
}

uint32_t GetAdapterMonitor(Cpu*, ComObject*) { return 0x00010001; }

uint32_t CreateDevice(Cpu* c, ComObject*) {
  uint32_t hwnd = arg(c, 3), behavior = arg(c, 4), pp = arg(c, 5), out = arg(c, 6);
  auto* dev = new Device();
  dev->d3d_ptr = arg(c, 0);
  dev->hwnd = hwnd;
  dev->behavior = behavior;
  for (int i = 0; i < 14; i++) dev->pp[i] = ld32(pp + 4 * (uint32_t)i);
  if (!dev->pp[0] || !dev->pp[1]) {
    int w, h;
    plat::get_window_size(&w, &h);
    dev->pp[0] = (uint32_t)w;
    dev->pp[1] = (uint32_t)h;
    st32(pp, dev->pp[0]);
    st32(pp + 4, dev->pp[1]);
  }
  if (dev->pp[2] == FMT_UNKNOWN) {
    dev->pp[2] = FMT_X8R8G8B8;
    st32(pp + 8, dev->pp[2]);
  }
  log(Log::Info, "CreateDevice: %ux%u %s, windowed=%u, autodepth=%u (%s), behavior=%08x, interval=%u", dev->pp[0], dev->pp[1],
      format_name(dev->pp[2]), dev->pp[8], dev->pp[9], format_name(dev->pp[10]), behavior, dev->pp[13]);
  // The game's x87 code then runs with the precision D3D9 selects (24-bit unless FPU_PRESERVE).
  if (!(behavior & 0x2)) {
    Cpu* cpu = c;
    cpu->fcw = (cpu->fcw & ~0x300u);
  }
  recorder_create_device(dev);
  dev->guest_ptr = expose(dev, iface_device());
  st32(out, dev->guest_ptr);
  return S_OK;
}

rt::ComInterface& iface_d3d9() {
  static rt::ComInterface i("IDirect3D9", {{"QueryInterface", 2, rt::com_QueryInterface_none},
                                           {"AddRef", 0, rt::com_AddRef},
                                           {"Release", 0, rt::com_Release},
                                           {"RegisterSoftwareDevice", 1, nullptr},
                                           {"GetAdapterCount", 0, GetAdapterCount},
                                           {"GetAdapterIdentifier", 3, GetAdapterIdentifier},
                                           {"GetAdapterModeCount", 2, GetAdapterModeCount},
                                           {"EnumAdapterModes", 4, EnumAdapterModes},
                                           {"GetAdapterDisplayMode", 2, GetAdapterDisplayMode},
                                           {"CheckDeviceType", 5, CheckDeviceType},
                                           {"CheckDeviceFormat", 6, CheckDeviceFormat},
                                           {"CheckDeviceMultiSampleType", 6, CheckDeviceMultiSampleType},
                                           {"CheckDepthStencilMatch", 5, CheckDepthStencilMatch},
                                           {"CheckDeviceFormatConversion", 4, ok},
                                           {"GetDeviceCaps", 3, GetDeviceCaps},
                                           {"GetAdapterMonitor", 1, GetAdapterMonitor},
                                           {"CreateDevice", 6, CreateDevice}});
  return serialized(i);
}

}  // namespace

uint32_t expose(ComObject* obj, rt::ComInterface& iface) { return rt::com_wrap(obj, iface); }
uint32_t guest_ptr_of(ComObject* obj) { return obj->wrappers.empty() ? 0 : obj->wrappers[0]; }

}  // namespace d3d

HLE_STDCALL("d3d9.dll", Direct3DCreate9, 1) {
  log(Log::Info, "Direct3DCreate9(SDK %u)", arg(c, 0));
  return rt::com_wrap(new d3d::Direct3D9(), d3d::iface_d3d9());
}
