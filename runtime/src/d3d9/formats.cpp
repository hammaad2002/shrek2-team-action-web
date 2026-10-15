#include "d3d9.h"

namespace d3d {

FormatInfo format_info(uint32_t fmt) {
  switch (fmt) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_A8B8G8R8: case FMT_X8B8G8R8: case FMT_D32: case FMT_D24S8:
    case FMT_D24X8: case FMT_D24X4S4: case FMT_R32F: case FMT_INDEX32:
      return {32, 0, fmt == FMT_D32 || fmt == FMT_D24S8 || fmt == FMT_D24X8 || fmt == FMT_D24X4S4, true};
    case FMT_R8G8B8:
      return {24, 0, false, true};
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_X4R4G4B4: case FMT_A8L8:
    case FMT_A8P8: case FMT_V8U8: case FMT_L16: case FMT_R16F: case FMT_INDEX16:
      return {16, 0, false, true};
    case FMT_D16: case FMT_D16_LOCKABLE: case FMT_D15S1:
      return {16, 0, true, true};
    case FMT_A8: case FMT_L8: case FMT_P8: case FMT_A4L4:
      return {8, 0, false, true};
    case FMT_A16B16G16R16F:
      return {64, 0, false, true};
    case FMT_A32B32G32R32F:
      return {128, 0, false, true};
    case FMT_DXT1:
      return {4, 8, false, true};
    case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
      return {8, 16, false, true};
    default:
      return {32, 0, false, false};
  }
}

const char* format_name(uint32_t fmt) {
  switch (fmt) {
    case FMT_A8R8G8B8: return "A8R8G8B8";
    case FMT_X8R8G8B8: return "X8R8G8B8";
    case FMT_R5G6B5: return "R5G6B5";
    case FMT_X1R5G5B5: return "X1R5G5B5";
    case FMT_A1R5G5B5: return "A1R5G5B5";
    case FMT_A4R4G4B4: return "A4R4G4B4";
    case FMT_A8: return "A8";
    case FMT_L8: return "L8";
    case FMT_A8L8: return "A8L8";
    case FMT_P8: return "P8";
    case FMT_DXT1: return "DXT1";
    case FMT_DXT2: return "DXT2";
    case FMT_DXT3: return "DXT3";
    case FMT_DXT4: return "DXT4";
    case FMT_DXT5: return "DXT5";
    case FMT_D16: return "D16";
    case FMT_D24S8: return "D24S8";
    case FMT_D24X8: return "D24X8";
    case FMT_D32: return "D32";
    case FMT_INDEX16: return "INDEX16";
    case FMT_INDEX32: return "INDEX32";
    default: return "?";
  }
}

uint32_t row_pitch(uint32_t fmt, uint32_t width) {
  FormatInfo f = format_info(fmt);
  if (f.block) return std::max<uint32_t>(1, (width + 3) / 4) * f.block;
  return width * f.bpp / 8;
}

uint32_t level_rows(uint32_t fmt, uint32_t height) {
  return format_info(fmt).block ? std::max<uint32_t>(1, (height + 3) / 4) : height;
}

void init_default_state(State& s) {
  s.rs.fill(0);
  // D3D9 default render states (values from the SDK documentation).
  s.rs[7] = 1;            // ZENABLE (TRUE with an auto depth buffer)
  s.rs[8] = 3;            // FILLMODE = SOLID
  s.rs[9] = 2;            // SHADEMODE = GOURAUD
  s.rs[14] = 1;           // ZWRITEENABLE
  s.rs[15] = 0;           // ALPHATESTENABLE
  s.rs[16] = 1;           // LASTPIXEL
  s.rs[19] = 2;           // SRCBLEND = ONE
  s.rs[20] = 1;           // DESTBLEND = ZERO
  s.rs[22] = 3;           // CULLMODE = CCW
  s.rs[23] = 4;           // ZFUNC = LESSEQUAL
  s.rs[24] = 0;           // ALPHAREF
  s.rs[25] = 8;           // ALPHAFUNC = ALWAYS
  s.rs[26] = 0;           // DITHERENABLE
  s.rs[27] = 0;           // ALPHABLENDENABLE
  s.rs[28] = 0;           // FOGENABLE
  s.rs[29] = 0;           // SPECULARENABLE
  s.rs[34] = 0;           // FOGCOLOR
  s.rs[35] = 0;           // FOGTABLEMODE = NONE
  s.rs[36] = 0;           // FOGSTART 0.0f
  s.rs[37] = 0x3f800000;  // FOGEND 1.0f
  s.rs[38] = 0x3f800000;  // FOGDENSITY 1.0f
  s.rs[48] = 0;           // RANGEFOGENABLE
  s.rs[52] = 0;           // STENCILENABLE
  s.rs[53] = 1;           // STENCILFAIL = KEEP
  s.rs[54] = 1;           // STENCILZFAIL = KEEP
  s.rs[55] = 1;           // STENCILPASS = KEEP
  s.rs[56] = 8;           // STENCILFUNC = ALWAYS
  s.rs[57] = 0;           // STENCILREF
  s.rs[58] = 0xffffffff;  // STENCILMASK
  s.rs[59] = 0xffffffff;  // STENCILWRITEMASK
  s.rs[60] = 0xffffffff;  // TEXTUREFACTOR
  s.rs[136] = 1;          // CLIPPING
  s.rs[137] = 1;          // LIGHTING
  s.rs[139] = 0;          // AMBIENT
  s.rs[140] = 0;          // FOGVERTEXMODE = NONE
  s.rs[141] = 1;          // COLORVERTEX
  s.rs[142] = 1;          // LOCALVIEWER
  s.rs[143] = 0;          // NORMALIZENORMALS
  s.rs[145] = 1;          // DIFFUSEMATERIALSOURCE = COLOR1
  s.rs[146] = 2;          // SPECULARMATERIALSOURCE = COLOR2
  s.rs[147] = 0;          // AMBIENTMATERIALSOURCE = MATERIAL
  s.rs[148] = 0;          // EMISSIVEMATERIALSOURCE = MATERIAL
  s.rs[151] = 0;          // VERTEXBLEND = DISABLE
  s.rs[152] = 0;          // CLIPPLANEENABLE
  s.rs[154] = 0x3f800000; // POINTSIZE 1.0f
  s.rs[155] = 0x3f800000; // POINTSIZE_MIN 1.0f
  s.rs[156] = 0;          // POINTSPRITEENABLE
  s.rs[157] = 0;          // POINTSCALEENABLE
  s.rs[158] = 0x3f800000; // POINTSCALE_A
  s.rs[161] = 1;          // MULTISAMPLEANTIALIAS
  s.rs[162] = 0xffffffff; // MULTISAMPLEMASK
  s.rs[166] = 0x42800000; // POINTSIZE_MAX 64.0f
  s.rs[168] = 0x0000000f; // COLORWRITEENABLE
  s.rs[171] = 1;          // BLENDOP = ADD
  s.rs[174] = 0;          // SCISSORTESTENABLE
  s.rs[176] = 0;          // ANTIALIASEDLINEENABLE
  s.rs[185] = 0;          // TWOSIDEDSTENCILMODE
  s.rs[186] = 1; s.rs[187] = 1; s.rs[188] = 1; s.rs[189] = 8;  // CCW stencil ops / func
  s.rs[190] = 0xf; s.rs[191] = 0xf; s.rs[192] = 0xf;           // COLORWRITEENABLE1..3
  s.rs[193] = 0xffffffff; // BLENDFACTOR
  s.rs[206] = 0;          // SEPARATEALPHABLENDENABLE
  s.rs[207] = 2;          // SRCBLENDALPHA = ONE
  s.rs[208] = 1;          // DESTBLENDALPHA = ZERO
  s.rs[209] = 1;          // BLENDOPALPHA = ADD
  for (int t = 0; t < 8; t++) {
    auto& ts = s.tss[t];
    ts.fill(0);
    ts[1] = t == 0 ? 4 : 1;   // COLOROP: MODULATE for stage 0, DISABLE otherwise
    ts[2] = 2;                // COLORARG1 = TEXTURE
    ts[3] = 1;                // COLORARG2 = CURRENT
    ts[4] = t == 0 ? 2 : 1;   // ALPHAOP: SELECTARG1 for stage 0, DISABLE otherwise
    ts[5] = 2;                // ALPHAARG1 = TEXTURE
    ts[6] = 1;                // ALPHAARG2 = CURRENT
    ts[11] = (uint32_t)t;     // TEXCOORDINDEX
    ts[26] = 1;               // COLORARG0 = CURRENT
    ts[27] = 1;               // ALPHAARG0 = CURRENT
    ts[28] = 1;               // RESULTARG = CURRENT
  }
  for (auto& ss : s.ss) {
    ss.fill(0);
    ss[1] = ss[2] = ss[3] = 1;  // ADDRESSU/V/W = WRAP
    ss[5] = 1;                  // MAGFILTER = POINT
    ss[6] = 1;                  // MINFILTER = POINT
    ss[7] = 0;                  // MIPFILTER = NONE
    ss[10] = 1;                 // MAXANISOTROPY
  }
  s.transforms.clear();
  s.material = Material{};
  s.lights.clear();
  s.light_enable.clear();
}

}  // namespace d3d
