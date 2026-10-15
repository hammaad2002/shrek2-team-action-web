// Fixed-function D3D9 vertex and pixel processing as generated GLSL ES 3.00.
//
// Matrices are uploaded in D3D's row-major layout, which GLSL reads as the transpose, so
// `M * v` in GLSL computes D3D's row-vector product `v * M`.
#include <cstdarg>
#include <cstdio>
#include <string>

#include "programs.h"

namespace gles {

int attr_location(uint32_t usage, uint32_t index) {
  switch (usage) {
    case 0:  // POSITION
    case 9:  // POSITIONT
      return index == 0 ? kLocPosition : kLocExtra;
    case 1: return kLocBlendWeight;
    case 2: return kLocBlendIndices;
    case 3: return index == 0 ? kLocNormal : -1;
    case 4: return kLocPSize;
    case 5: return index < 8 ? kLocTex0 + (int)index : -1;
    case 10: return index == 0 ? kLocColor0 : index == 1 ? kLocColor1 : -1;
    case 6:   // TANGENT
    case 11:  // FOG
      return kLocExtra;
    default: return -1;
  }
}

namespace {

struct Out {
  std::string s;
  void operator()(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s += buf;
  }
};

const char* kHeader = "#version 300 es\nprecision highp float;\nprecision highp int;\n";

bool has(const ProgKey& k, int loc) { return (k.attr_mask >> loc) & 1; }

// Attribute read with the D3DCOLOR swizzle applied.
std::string attr(const ProgKey& k, int loc) {
  char buf[32];
  snprintf(buf, sizeof buf, (k.bgra_mask >> loc) & 1 ? "a%d.zyxw" : "a%d", loc);
  return buf;
}

}  // namespace

// Varyings shared by every program: colours, one texture coordinate per stage, fog.
static void declare_vs_outputs(Out& o) { o("out vec4 v_c0;\nout vec4 v_c1;\nout vec4 v_t[8];\nout float v_fog;\n"); }

// D3D clip space -> GL: flip Y (render targets are stored top row first), move z from [0,w] to
// [-w,w], and apply D3D9's half-pixel offset (pixel centres at integer coordinates).
static void emit_position(Out& o, const char* c) {
  o("  gl_Position = vec4(%s.x + %s.w / u_vp.z, -%s.y + %s.w / u_vp.w, 2.0 * %s.z - %s.w, %s.w);\n", c, c, c, c, c, c, c);
}

std::string ff_vertex_glsl(const ProgKey& k) {
  Out o;
  o.s = kHeader;
  for (int loc = 0; loc < 16; loc++)
    if (has(k, loc)) o("layout(location = %d) in vec4 a%d;\n", loc, loc);
  o("uniform vec4 u_vp;\n");  // viewport x, y, w, h
  bool rhw = k.vs_kind == 1;
  if (!rhw) {
    o("uniform mat4 u_wv[4];\nuniform mat3 u_nm[4];\nuniform mat4 u_proj;\n");
    if (k.lighting) o("uniform vec4 u_mat[5];\nuniform vec4 u_amb;\nuniform vec4 u_light[56];\n");
    if (k.ntc) o("uniform mat4 u_tm[8];\n");
  }
  if (k.vfog >= 2 && k.vfog <= 4) o("uniform vec4 u_fog;\n");
  if (k.points) o("uniform vec4 u_pt;\nuniform vec4 u_ptk;\n");
  declare_vs_outputs(o);
  o("void main() {\n");
  std::string c0 = has(k, kLocColor0) ? attr(k, kLocColor0) : "vec4(1.0)";
  std::string c1 = has(k, kLocColor1) ? attr(k, kLocColor1) : "vec4(0.0)";
  if (rhw) {
    o("  float w = a0.w == 0.0 ? 1.0 : 1.0 / a0.w;\n");
    o("  vec2 ndc = (a0.xy + 0.5 - u_vp.xy) / u_vp.zw * 2.0 - 1.0;\n");
    o("  gl_Position = vec4(ndc * w, (a0.z * 2.0 - 1.0) * w, w);\n");
    o("  v_c0 = %s;\n  v_c1 = %s;\n", c0.c_str(), c1.c_str());
    for (int i = 0; i < k.ntc; i++) {
      if (k.tc_src[i] < 8 && k.tc_dims[i]) {
        int d = k.tc_dims[i];
        static const char* pad[5] = {"", "vec4(a%d.x, 1.0, 0.0, 0.0)", "vec4(a%d.xy, 1.0, 0.0)", "vec4(a%d.xyz, 1.0)", "a%d"};
        char buf[64];
        snprintf(buf, sizeof buf, pad[d], kLocTex0 + k.tc_src[i]);
        o("  v_t[%d] = %s;\n", i, buf);
      } else {
        o("  v_t[%d] = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
      }
    }
    if (k.vfog == 1) o("  v_fog = a0.z;\n");
    else if (k.vfog) o("  v_fog = %s;\n", has(k, kLocColor1) ? (c1 + ".a").c_str() : "1.0");
    if (k.points) o("  gl_PointSize = clamp(%s, u_pt.y, u_pt.z);\n", has(k, kLocPSize) ? "a4.x" : "u_pt.x");
    o("}\n");
    return o.s;
  }

  o("  vec4 p = %s;\n", k.pos4 ? "a0" : "vec4(a0.xyz, 1.0)");
  bool need_normal = k.lighting || [&] {
    for (int i = 0; i < k.ntc; i++)
      if (k.tc_src[i] == 0x10 || k.tc_src[i] >= 0x30) return true;
    return false;
  }();
  bool nrm = has(k, kLocNormal);
  if (k.blend) {
    // Vertex blending in view space: the last weight is 1 - sum(others).
    o("  float wsum = 0.0;\n  vec4 pv = vec4(0.0);\n  vec3 n = vec3(0.0);\n");
    for (int i = 0; i < k.blend; i++) {
      if (i < k.blend - 1) o("  float w%d = a1[%d];\n  wsum += w%d;\n", i, i, i);
      else o("  float w%d = 1.0 - wsum;\n", i);
      o("  pv += (u_wv[%d] * p) * w%d;\n", i, i);
      if (nrm) o("  n += (u_nm[%d] * a3.xyz) * w%d;\n", i, i);
    }
  } else {
    o("  vec4 pv = u_wv[0] * p;\n");
    o("  vec3 n = %s;\n", nrm ? "u_nm[0] * a3.xyz" : "vec3(0.0)");
  }
  if (need_normal && k.normalize) o("  if (dot(n, n) > 0.0) n = normalize(n);\n");
  o("  vec4 c = u_proj * pv;\n");
  emit_position(o, "c");

  if (k.lighting) {
    auto src = [&](int s, const char* mat) -> std::string {
      if (s == 1) return c0;
      if (s == 2) return c1;
      return mat;
    };
    o("  vec4 md = %s;\n  vec4 ma = %s;\n  vec4 ms = %s;\n  vec4 me = %s;\n", src(k.dsrc, "u_mat[0]").c_str(), src(k.asrc, "u_mat[1]").c_str(),
      src(k.ssrc, "u_mat[2]").c_str(), src(k.esrc, "u_mat[3]").c_str());
    o("  vec3 amb = u_amb.rgb, dif = vec3(0.0), spe = vec3(0.0);\n");
    o("  vec3 P = pv.xyz;\n");
    if (k.specular) o("  vec3 V = %s;\n", k.local_viewer ? "-normalize(P)" : "vec3(0.0, 0.0, -1.0)");
    for (int i = 0; i < k.nlights; i++) {
      int b = i * 7;
      o("  {\n");
      if (k.light_type[i] == 3) {  // directional
        o("    vec3 L = -u_light[%d].xyz;\n    float att = 1.0;\n", b + 4);
      } else {
        o("    vec3 d = u_light[%d].xyz - P;\n    float dist = length(d);\n    vec3 L = d / max(dist, 1e-20);\n", b + 3);
        o("    float att = dist <= u_light[%d].w ? 1.0 / (u_light[%d].x + u_light[%d].y * dist + u_light[%d].z * dist * dist) : 0.0;\n",
          b + 3, b + 5, b + 5, b + 5);
        if (k.light_type[i] == 2) {  // spot
          o("    float rho = dot(-L, u_light[%d].xyz);\n", b + 4);
          o("    float spot = rho > u_light[%d].w ? 1.0 : rho <= u_light[%d].x ? 0.0 : "
            "pow(max((rho - u_light[%d].x) / max(u_light[%d].w - u_light[%d].x, 1e-20), 0.0), u_light[%d].w);\n",
            b + 5, b + 6, b + 6, b + 5, b + 6, b + 4);
          o("    att *= spot;\n");
        }
      }
      o("    float ndl = dot(n, L);\n");
      o("    amb += u_light[%d].rgb * att;\n", b + 2);
      o("    if (ndl > 0.0) {\n      dif += u_light[%d].rgb * (ndl * att);\n", b);
      if (k.specular) {
        o("      vec3 H = normalize(L + V);\n      float ndh = dot(n, H);\n");
        o("      if (ndh > 0.0) spe += u_light[%d].rgb * (pow(ndh, u_mat[4].x) * att);\n", b + 1);
      }
      o("    }\n  }\n");
    }
    o("  v_c0 = vec4(clamp(me.rgb + ma.rgb * amb + md.rgb * dif, 0.0, 1.0), clamp(md.a, 0.0, 1.0));\n");
    o("  v_c1 = vec4(clamp(ms.rgb * spe, 0.0, 1.0), %s);\n", has(k, kLocColor1) ? (c1 + ".a").c_str() : "1.0");
  } else {
    o("  v_c0 = %s;\n  v_c1 = %s;\n", c0.c_str(), c1.c_str());
  }

  for (int i = 0; i < k.ntc; i++) {
    uint8_t s = k.tc_src[i];
    o("  {\n    vec4 tc;\n");
    if (s < 8) {
      int d = k.tc_dims[i];
      int loc = kLocTex0 + s;
      if (!d) o("    tc = vec4(0.0, 0.0, 0.0, 1.0);\n");
      else if (d == 1) o("    tc = vec4(a%d.x, 1.0, 0.0, 0.0);\n", loc);
      else if (d == 2) o("    tc = vec4(a%d.xy, 1.0, 0.0);\n", loc);
      else if (d == 3) o("    tc = vec4(a%d.xyz, 1.0);\n", loc);
      else o("    tc = a%d;\n", loc);
    } else if (s == 0x10) {
      o("    tc = vec4(n, 1.0);\n");
    } else if (s == 0x20) {
      o("    tc = vec4(pv.xyz, 1.0);\n");
    } else if (s == 0x30) {
      o("    tc = vec4(reflect(%s, n), 1.0);\n", k.local_viewer ? "normalize(pv.xyz)" : "vec3(0.0, 0.0, 1.0)");
    } else {
      o("    vec3 r = reflect(normalize(pv.xyz), n);\n    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));\n");
      o("    tc = vec4(r.x / m + 0.5, r.y / m + 0.5, 0.0, 1.0);\n");
    }
    if (k.tc_count[i]) o("    tc = u_tm[%d] * tc;\n", i);
    o("    v_t[%d] = tc;\n  }\n", i);
  }

  switch (k.vfog) {
    case 1: o("  v_fog = %s;\n", k.wfog ? "c.w" : "c.z / c.w"); break;
    case 2:
    case 3:
    case 4:
      o("  float fd = %s;\n", k.range_fog ? "length(pv.xyz)" : "abs(pv.z)");
      if (k.vfog == 2) o("  v_fog = exp(-u_fog.z * fd);\n");
      else if (k.vfog == 3) o("  v_fog = exp(-(u_fog.z * fd) * (u_fog.z * fd));\n");
      else o("  v_fog = (u_fog.y - fd) / (u_fog.y - u_fog.x);\n");
      break;
    case 5: o("  v_fog = v_c1.a;\n"); break;
    default: break;
  }
  if (k.points) {
    o("  float ps = %s;\n", has(k, kLocPSize) ? "a4.x" : "u_pt.x");
    if (k.point_scale) o("  float de = length(pv.xyz);\n  ps = u_vp.w * ps * inversesqrt(max(u_ptk.x + u_ptk.y * de + u_ptk.z * de * de, 1e-20));\n");
    o("  gl_PointSize = clamp(ps, u_pt.y, u_pt.z);\n");
  }
  o("}\n");
  return o.s;
}

// ---- Pixel stage ----------------------------------------------------------------------------

namespace {

enum { TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X, TOP_MODULATE4X, TOP_ADD, TOP_ADDSIGNED,
       TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH, TOP_BLENDDIFFUSEALPHA, TOP_BLENDTEXTUREALPHA, TOP_BLENDFACTORALPHA,
       TOP_BLENDTEXTUREALPHAPM, TOP_BLENDCURRENTALPHA, TOP_PREMODULATE, TOP_MODULATEALPHA_ADDCOLOR, TOP_MODULATECOLOR_ADDALPHA,
       TOP_MODULATEINVALPHA_ADDCOLOR, TOP_MODULATEINVCOLOR_ADDALPHA, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE, TOP_DOTPRODUCT3,
       TOP_MULTIPLYADD, TOP_LERP };

// D3DTA argument as a vec4 expression.
std::string arg_vec(uint8_t a, int stage) {
  std::string v;
  switch (a & 7) {
    case 0: v = "v_c0"; break;
    case 1: v = "cur"; break;
    case 2: v = "tex"; break;
    case 3: v = "u_tf"; break;
    case 4: v = "v_c1"; break;
    case 5: v = "tmp"; break;
    case 6: v = "u_tc[" + std::to_string(stage) + "]"; break;
    default: v = "cur"; break;
  }
  if (a & 0x20) v = "(" + v + ").aaaa";
  if (a & 0x10) v = "(1.0 - " + v + ")";
  return v;
}

bool uses_texture(uint8_t op, const uint8_t* args) {
  if (op == TOP_DISABLE) return false;
  if (op == TOP_BLENDTEXTUREALPHA || op == TOP_BLENDTEXTUREALPHAPM) return true;
  for (int i = 0; i < 3; i++)
    if ((args[i] & 7) == 2) return true;
  return false;
}

// Combine arguments with a texture operation. `sw` selects the components (".rgb" or ".a").
std::string combine(uint8_t op, const std::string& a0, const std::string& a1, const std::string& a2, const char* sw, bool color) {
  auto A = [&](const std::string& x) { return "(" + x + ")" + sw; };
  std::string s1 = A(a1), s2 = A(a2), s0 = A(a0);
  std::string alpha1 = "(" + a1 + ").a";
  switch (op) {
    case TOP_SELECTARG1: return s1;
    case TOP_SELECTARG2: return s2;
    case TOP_MODULATE: return s1 + " * " + s2;
    case TOP_MODULATE2X: return s1 + " * " + s2 + " * 2.0";
    case TOP_MODULATE4X: return s1 + " * " + s2 + " * 4.0";
    case TOP_ADD: return s1 + " + " + s2;
    case TOP_ADDSIGNED: return s1 + " + " + s2 + " - 0.5";
    case TOP_ADDSIGNED2X: return "(" + s1 + " + " + s2 + " - 0.5) * 2.0";
    case TOP_SUBTRACT: return s1 + " - " + s2;
    case TOP_ADDSMOOTH: return s1 + " + " + s2 + " * (1.0 - " + s1 + ")";
    case TOP_BLENDDIFFUSEALPHA: return "mix(" + s2 + ", " + s1 + ", v_c0.a)";
    case TOP_BLENDTEXTUREALPHA: return "mix(" + s2 + ", " + s1 + ", tex.a)";
    case TOP_BLENDFACTORALPHA: return "mix(" + s2 + ", " + s1 + ", u_tf.a)";
    case TOP_BLENDTEXTUREALPHAPM: return s1 + " + " + s2 + " * (1.0 - tex.a)";
    case TOP_BLENDCURRENTALPHA: return "mix(" + s2 + ", " + s1 + ", cur.a)";
    case TOP_PREMODULATE: return s1;
    case TOP_MODULATEALPHA_ADDCOLOR: return color ? s1 + " + " + alpha1 + " * " + s2 : s1;
    case TOP_MODULATECOLOR_ADDALPHA: return color ? s1 + " * " + s2 + " + " + alpha1 : s1;
    case TOP_MODULATEINVALPHA_ADDCOLOR: return color ? "(1.0 - " + alpha1 + ") * " + s2 + " + " + s1 : s1;
    case TOP_MODULATEINVCOLOR_ADDALPHA: return color ? "(1.0 - " + s1 + ") * " + s2 + " + " + alpha1 : s1;
    case TOP_DOTPRODUCT3: {
      std::string d = "(4.0 * dot((" + a1 + ").rgb - 0.5, (" + a2 + ").rgb - 0.5))";
      return color ? "vec3" + d : d;
    }
    case TOP_MULTIPLYADD: return s0 + " + " + s1 + " * " + s2;
    case TOP_LERP: return "mix(" + s2 + ", " + s1 + ", " + s0 + ")";
    case TOP_BUMPENVMAP:
    case TOP_BUMPENVMAPLUMINANCE:
    default: return std::string("cur") + sw;
  }
}

}  // namespace

std::string ff_fragment_glsl(const ProgKey& k) {
  Out o;
  o.s = kHeader;
  o("in vec4 v_c0;\nin vec4 v_c1;\nin vec4 v_t[8];\nin float v_fog;\n");
  o("uniform vec4 u_tf;\nuniform vec4 u_tc[8];\nuniform vec4 u_fogc;\nuniform vec4 u_fogp;\nuniform float u_aref;\n");
  for (int i = 0; i < k.nstages; i++) {
    if (k.tex_kind[i] == 1) o("uniform sampler2D s%d;\n", i);
    else if (k.tex_kind[i] == 2) o("uniform samplerCube s%d;\n", i);
  }
  o("out vec4 o_color;\n");
  o("void main() {\n  vec4 cur = v_c0;\n  vec4 tmp = vec4(0.0);\n  vec4 tex;\n");
  for (int i = 0; i < k.nstages; i++) {
    uint8_t cop = k.cop[i], aop = k.aop[i];
    if (cop == TOP_DISABLE) break;
    o("  {\n");
    bool tex_used = uses_texture(cop, k.ca[i]) || uses_texture(aop, k.aa[i]);
    if (tex_used) {
      int t = k.tc_index[i];
      char coord[96];
      if (k.point_sprite) snprintf(coord, sizeof coord, "vec2(gl_PointCoord.x, 1.0 - gl_PointCoord.y)");
      else if (k.tex_kind[i] == 2) snprintf(coord, sizeof coord, "v_t[%d].xyz", t);
      else if (k.tex_proj[i] == 3) snprintf(coord, sizeof coord, "v_t[%d].xy / v_t[%d].z", t, t);
      else if (k.tex_proj[i] == 4) snprintf(coord, sizeof coord, "v_t[%d].xy / v_t[%d].w", t, t);
      else snprintf(coord, sizeof coord, "v_t[%d].xy", t);
      if (k.tex_kind[i]) o("    tex = texture(s%d, %s);\n", i, coord);
      else o("    tex = vec4(1.0);\n");  // no texture bound
    }
    std::string c0 = arg_vec(k.ca[i][0], i), c1 = arg_vec(k.ca[i][1], i), c2 = arg_vec(k.ca[i][2], i);
    std::string a0 = arg_vec(k.aa[i][0], i), a1 = arg_vec(k.aa[i][1], i), a2 = arg_vec(k.aa[i][2], i);
    o("    vec3 rc = clamp(%s, 0.0, 1.0);\n", combine(cop, c0, c1, c2, ".rgb", true).c_str());
    if (cop == TOP_DOTPRODUCT3) o("    float ra = rc.r;\n");  // DOTPRODUCT3 replicates into alpha
    else if (aop == TOP_DISABLE) o("    float ra = cur.a;\n");
    else o("    float ra = clamp(%s, 0.0, 1.0);\n", combine(aop, a0, a1, a2, ".a", false).c_str());
    o("    %s = vec4(rc, ra);\n  }\n", k.res[i] == 5 ? "tmp" : "cur");
  }
  if (k.spec_add) o("  cur.rgb = clamp(cur.rgb + v_c1.rgb, 0.0, 1.0);\n");
  if (k.pfog) {
    switch (k.pfog) {
      case 1: o("  float f = clamp(v_fog, 0.0, 1.0);\n"); break;
      case 2: o("  float f = clamp(exp(-u_fogp.z * v_fog), 0.0, 1.0);\n"); break;
      case 3: o("  float f = clamp(exp(-(u_fogp.z * v_fog) * (u_fogp.z * v_fog)), 0.0, 1.0);\n"); break;
      default: o("  float f = clamp((u_fogp.y - v_fog) / (u_fogp.y - u_fogp.x), 0.0, 1.0);\n"); break;
    }
    o("  cur.rgb = mix(u_fogc.rgb, cur.rgb, f);\n");
  }
  if (k.alpha_func) {
    static const char* cmp[9] = {"", "false", "a < u_aref", "a == u_aref", "a <= u_aref", "a > u_aref", "a != u_aref", "a >= u_aref", "true"};
    o("  float a = floor(cur.a * 255.0 + 0.5);\n");
    o("  if (!(%s)) discard;\n", cmp[k.alpha_func < 9 ? k.alpha_func : 8]);
  }
  o("  o_color = cur;\n}\n");
  return o.s;
}

}  // namespace gles
