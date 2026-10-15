// GLSL generation for the D3D9 pipeline: fixed-function vertex/pixel processing and translated
// D3D shader bytecode, all as GLSL ES 3.00 (the WebGL 2 shading language).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gles {

// Attribute locations, by D3D vertex element usage.
enum AttrLoc : int {
  kLocPosition = 0, kLocBlendWeight = 1, kLocBlendIndices = 2, kLocNormal = 3, kLocPSize = 4, kLocColor0 = 5,
  kLocColor1 = 6, kLocTex0 = 7, /* .. kLocTex0 + 7 = 14 */ kLocExtra = 15
};
int attr_location(uint32_t usage, uint32_t index);  // -1 when unmapped

// Everything that changes generated code. Zero-initialised with memset (compared bytewise).
struct ProgKey {
  uint32_t vs_id;        // translated vertex shader (vs_kind 2)
  uint16_t bgra_mask;    // attribute locations fed with D3DCOLOR (BGRA) data
  uint16_t attr_mask;    // attribute locations present in the vertex layout
  uint8_t vs_kind;       // 0 fixed function, 1 fixed function with pre-transformed (RHW) positions, 2 vertex shader
  uint8_t pos4;          // position has a meaningful w (XYZW)
  uint8_t lighting, nlights, specular, local_viewer, normalize;
  uint8_t light_type[8];
  uint8_t dsrc, asrc, ssrc, esrc;  // material sources: 0 material, 1 vertex diffuse, 2 vertex specular
  uint8_t vfog;          // vertex fog output: 0 none, 1 depth (table fog), 2 exp, 3 exp2, 4 linear, 5 specular alpha / oFog
  uint8_t range_fog, wfog;
  uint8_t blend;         // number of blend matrices (0 = off)
  uint8_t points, point_scale;
  uint8_t ntc;           // texture coordinate sets output by the vertex stage
  uint8_t tc_src[8];     // per stage: input set (0-7), or generated: 0x10 normal, 0x20 position, 0x30 reflection, 0x40 sphere map
  uint8_t tc_dims[8];    // components in that input set (0 = missing)
  uint8_t tc_count[8];   // texture transform output components (0 = no transform)
  // Pixel stage.
  uint8_t nstages;
  uint8_t cop[8], ca[8][3], aop[8], aa[8][3], res[8];
  uint8_t tex_kind[8];   // 0 none, 1 2D, 2 cube
  uint8_t tex_proj[8];   // projective divide by component count (3 or 4), 0 = none
  uint8_t tc_index[8];   // varying sampled by the stage
  uint8_t spec_add;      // add the specular colour after texturing
  uint8_t pfog;          // 0 none, 1 factor from v_fog, 2 exp, 3 exp2, 4 linear (table fog from depth)
  uint8_t alpha_func;    // D3DCMP, 0 = alpha test off
  uint8_t point_sprite;
};

std::string ff_vertex_glsl(const ProgKey& k);
std::string ff_fragment_glsl(const ProgKey& k);

// Translate D3D vertex shader bytecode (vs_1_1 .. vs_2_0, no flow control yet).
struct VsInfo {
  uint32_t nconst = 0;  // size of the c[] uniform array
  bool ok = false;
};
std::string sm_vertex_glsl(const std::vector<uint32_t>& tokens, const ProgKey& k, VsInfo* info, std::string* err);

}  // namespace gles
