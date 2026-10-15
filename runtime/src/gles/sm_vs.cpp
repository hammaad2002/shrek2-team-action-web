// D3D9 vertex shader bytecode (vs_1_1 / vs_2_0 without flow control) -> GLSL ES 3.00.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "programs.h"

namespace gles {

namespace {

enum Reg { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_RASTOUT = 4, R_ATTROUT = 5, R_TEXCRDOUT = 6, R_CONSTINT = 7,
           R_CONSTBOOL = 14, R_LOOP = 15 };

enum Op { OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT, OP_SGE,
          OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2, OP_DCL = 31, OP_POW = 32,
          OP_CRS = 33, OP_SGN = 34, OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37, OP_MOVA = 46, OP_EXPP = 78, OP_LOGP = 79,
          OP_DEF = 81, OP_COMMENT = 0xfffe, OP_END = 0xffff };

uint32_t reg_type(uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
uint32_t reg_num(uint32_t t) { return t & 0x7ff; }

// Number of parameter tokens for shader model 1 opcodes (SM2+ encode the length).
int sm1_params(uint32_t op) {
  switch (op) {
    case OP_NOP: return 0;
    case OP_MOV: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_LIT: case OP_FRC: case OP_EXPP: case OP_LOGP:
    case OP_ABS: case OP_NRM: case OP_MOVA: case OP_DCL:
      return 2;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE:
    case OP_DST: case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: case OP_POW: case OP_CRS:
      return 3;
    case OP_MAD: case OP_LRP: case OP_SGN: return 4;
    case OP_DEF: return 5;
    default: return -1;
  }
}

struct Translator {
  const std::vector<uint32_t>& t;
  const ProgKey& key;
  int major = 1;
  std::string body, err;
  std::map<uint32_t, std::string> defs;  // c# -> literal
  std::map<uint32_t, int> input_loc;     // v# -> attribute location
  uint32_t max_const = 0;
  bool relative = false;
  std::set<uint32_t> temps;

  Translator(const std::vector<uint32_t>& tokens, const ProgKey& k) : t(tokens), key(k) {}

  std::string reg_name(uint32_t tok, uint32_t rel_tok, bool has_rel, int offset = 0) {
    uint32_t type = reg_type(tok), n = reg_num(tok) + (uint32_t)offset;
    char buf[64];
    switch (type) {
      case R_TEMP: temps.insert(n); snprintf(buf, sizeof buf, "r%u", n); return buf;
      case R_INPUT: snprintf(buf, sizeof buf, "v%u", n); return buf;
      case R_CONST:
        if (has_rel) {
          relative = true;
          snprintf(buf, sizeof buf, "c[clamp(a0.x + %u, 0, 255)]", n);
          return buf;
        }
        if (defs.count(n)) return "cdef" + std::to_string(n);
        max_const = std::max(max_const, n + 1);
        snprintf(buf, sizeof buf, "c[%u]", n);
        return buf;
      case R_ADDR: return "vec4(a0)";
      case R_RASTOUT: return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
      case R_ATTROUT: return n == 0 ? "oD0" : "oD1";
      case R_TEXCRDOUT: snprintf(buf, sizeof buf, "oT[%u]", n & 7); return buf;
      default: err = "unsupported register type " + std::to_string(type); return "vec4(0.0)";
    }
    (void)rel_tok;
  }

  // Source operand with swizzle and modifier, as a vec4 expression.
  std::string src(size_t& i, int offset = 0) {
    uint32_t tok = t[i++];
    bool has_rel = (tok >> 13) & 1;
    uint32_t rel = 0;
    if (has_rel && major >= 2) rel = t[i++];
    std::string r = reg_name(tok, rel, has_rel, offset);
    uint32_t sw = (tok >> 16) & 0xff;
    if (sw != 0xe4) {
      static const char comp[4] = {'x', 'y', 'z', 'w'};
      r += '.';
      for (int k = 0; k < 4; k++) r += comp[(sw >> (2 * k)) & 3];
    }
    switch ((tok >> 24) & 0xf) {
      case 0: return r;
      case 1: return "(-" + r + ")";
      case 2: return "(" + r + " - 0.5)";
      case 3: return "(-(" + r + " - 0.5))";
      case 4: return "((" + r + " - 0.5) * 2.0)";
      case 5: return "(-((" + r + " - 0.5) * 2.0))";
      case 6: return "(1.0 - " + r + ")";
      case 7: return "(" + r + " * 2.0)";
      case 8: return "(-(" + r + " * 2.0))";
      case 11: return "abs(" + r + ")";
      case 12: return "(-abs(" + r + "))";
      default: err = "unsupported source modifier"; return r;
    }
  }

  struct Dst {
    std::string name, mask;
    bool sat = false, addr = false;
    uint32_t mbits = 0xf;
  };

  Dst dst(size_t& i) {
    uint32_t tok = t[i++];
    Dst d;
    if ((tok >> 13) & 1) err = "relative destination";
    d.addr = reg_type(tok) == R_ADDR;
    d.name = d.addr ? "a0" : reg_name(tok, 0, false);
    d.mbits = (tok >> 16) & 0xf;
    if (d.mbits != 0xf) {
      d.mask = ".";
      for (int k = 0; k < 4; k++)
        if (d.mbits & (1u << k)) d.mask += "xyzw"[k];
    }
    d.sat = ((tok >> 20) & 1) != 0;
    return d;
  }

  void assign(const Dst& d, const std::string& expr) {
    std::string e = d.sat ? "clamp(" + expr + ", 0.0, 1.0)" : expr;
    if (d.addr) {
      // vs_1_1 mov a0.x floors; mova (vs_2_0) rounds to nearest.
      body += "  a0" + d.mask + " = ivec4(floor(" + e + (major >= 2 ? " + 0.5" : "") + "))" + d.mask + ";\n";
      return;
    }
    body += "  " + d.name + d.mask + " = (" + e + ")" + d.mask + ";\n";
  }

  bool run(VsInfo* info) {
    if (t.empty() || (t[0] & 0xffff0000) != 0xfffe0000) {
      err = "not a vertex shader";
      return false;
    }
    major = (int)((t[0] >> 8) & 0xff);
    if (major > 2) {
      err = "vs_3_0 not supported";
      return false;
    }
    size_t i = 1;
    while (i < t.size() && err.empty()) {
      uint32_t tok = t[i++];
      uint32_t op = tok & 0xffff;
      if (op == OP_END) break;
      if (op == OP_COMMENT) {
        i += (tok >> 16) & 0x7fff;
        continue;
      }
      size_t next;
      if (major >= 2) next = i + ((tok >> 24) & 0xf);
      else {
        int n = sm1_params(op);
        if (n < 0) {
          err = "unsupported opcode " + std::to_string(op);
          break;
        }
        next = i + (size_t)n;
      }
      switch (op) {
        case OP_NOP: break;
        case OP_DCL: {
          uint32_t usage = t[i] & 0x1f, index = (t[i] >> 16) & 0xf;
          uint32_t r = t[i + 1];
          if (reg_type(r) == R_INPUT) {
            int loc = attr_location(usage, index);
            if (loc < 0) err = "unsupported input usage " + std::to_string(usage);
            input_loc[reg_num(r)] = loc;
          }
          break;
        }
        case OP_DEF: {
          uint32_t r = reg_num(t[i]);
          float f[4];
          memcpy(f, &t[i + 1], 16);
          char buf[160];
          snprintf(buf, sizeof buf, "vec4(%.9g, %.9g, %.9g, %.9g)", f[0], f[1], f[2], f[3]);
          defs[r] = buf;
          break;
        }
        case OP_MOV: case OP_MOVA: { Dst d = dst(i); assign(d, src(i)); break; }
        case OP_ADD: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, a + " + " + b); break; }
        case OP_SUB: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, a + " - " + b); break; }
        case OP_MUL: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, a + " * " + b); break; }
        case OP_MAD: { Dst d = dst(i); std::string a = src(i), b = src(i), c = src(i); assign(d, a + " * " + b + " + " + c); break; }
        case OP_RCP: { Dst d = dst(i); std::string a = src(i); assign(d, "vec4(1.0 / (" + a + ").x)"); break; }
        case OP_RSQ: { Dst d = dst(i); std::string a = src(i); assign(d, "vec4(inversesqrt(abs((" + a + ").x)))"); break; }
        case OP_DP3: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "vec4(dot((" + a + ").xyz, (" + b + ").xyz))"); break; }
        case OP_DP4: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "vec4(dot(" + a + ", " + b + "))"); break; }
        case OP_MIN: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "min(" + a + ", " + b + ")"); break; }
        case OP_MAX: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "max(" + a + ", " + b + ")"); break; }
        case OP_SLT: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "vec4(lessThan(" + a + ", " + b + "))"); break; }
        case OP_SGE: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "vec4(greaterThanEqual(" + a + ", " + b + "))"); break; }
        case OP_EXP: { Dst d = dst(i); std::string a = src(i); assign(d, "vec4(exp2((" + a + ").x))"); break; }
        case OP_LOG: { Dst d = dst(i); std::string a = src(i); assign(d, "vec4(log2(abs((" + a + ").x)))"); break; }
        case OP_EXPP: {
          Dst d = dst(i);
          std::string a = src(i);
          assign(d, "vec4(exp2(floor((" + a + ").x)), fract((" + a + ").x), exp2((" + a + ").x), 1.0)");
          break;
        }
        case OP_LOGP: { Dst d = dst(i); std::string a = src(i); assign(d, "vec4(log2(abs((" + a + ").x)))"); break; }
        case OP_LIT: {
          Dst d = dst(i);
          std::string a = src(i);
          assign(d, "vec4(1.0, max((" + a + ").x, 0.0), ((" + a + ").x > 0.0 && (" + a + ").y > 0.0) ? pow((" + a + ").y, clamp((" + a +
                        ").w, -127.9961, 127.9961)) : 0.0, 1.0)");
          break;
        }
        case OP_DST: {
          Dst d = dst(i);
          std::string a = src(i), b = src(i);
          assign(d, "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)");
          break;
        }
        case OP_LRP: { Dst d = dst(i); std::string a = src(i), b = src(i), c = src(i); assign(d, "mix(" + c + ", " + b + ", " + a + ")"); break; }
        case OP_FRC: { Dst d = dst(i); assign(d, "fract(" + src(i) + ")"); break; }
        case OP_POW: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "vec4(pow(abs((" + a + ").x), (" + b + ").x))"); break; }
        case OP_CRS: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "vec4(cross((" + a + ").xyz, (" + b + ").xyz), 0.0)"); break; }
        case OP_SGN: { Dst d = dst(i); std::string a = src(i); assign(d, "sign(" + a + ")"); break; }
        case OP_ABS: { Dst d = dst(i); assign(d, "abs(" + src(i) + ")"); break; }
        case OP_NRM: { Dst d = dst(i); std::string a = src(i); assign(d, "vec4(normalize((" + a + ").xyz), (" + a + ").w)"); break; }
        case OP_SINCOS: { Dst d = dst(i); std::string a = src(i); assign(d, "vec4(cos((" + a + ").x), sin((" + a + ").x), 0.0, 0.0)"); break; }
        case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
          Dst d = dst(i);
          size_t s0 = i;
          std::string a = src(i);
          int rows = op == OP_M4x4 || op == OP_M3x4 ? 4 : op == OP_M3x2 ? 2 : 3;
          bool dot4 = op == OP_M4x4 || op == OP_M4x3;
          size_t s1 = i;
          for (int r = 0; r < rows; r++) {
            if (!(d.mbits & (1u << r))) continue;
            size_t j = s1;
            std::string b = src(j, r);
            std::string e = dot4 ? "dot(" + a + ", " + b + ")" : "dot((" + a + ").xyz, (" + b + ").xyz)";
            std::string c = std::string(".") + "xyzw"[r];
            body += "  " + d.name + c + " = " + (d.sat ? "clamp(" + e + ", 0.0, 1.0)" : e) + ";\n";
            i = j;
          }
          (void)s0;
          break;
        }
        default:
          err = "unsupported opcode " + std::to_string(op);
          break;
      }
      i = next;
    }
    if (!err.empty()) return false;
    info->nconst = relative ? 256 : max_const;
    info->ok = true;
    return true;
  }

  std::string glsl(const VsInfo& info) {
    std::string s = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    for (auto& [v, loc] : input_loc) s += "layout(location = " + std::to_string(loc) + ") in vec4 a" + std::to_string(loc) + ";\n";
    if (info.nconst) s += "uniform vec4 c[" + std::to_string(info.nconst) + "];\n";
    s += "uniform vec4 u_vp;\n";
    s += "out vec4 v_c0;\nout vec4 v_c1;\nout vec4 v_t[8];\nout float v_fog;\n";
    s += "void main() {\n";
    for (auto& [n, lit] : defs) s += "  const vec4 cdef" + std::to_string(n) + " = " + lit + ";\n";
    for (auto& [v, loc] : input_loc) {
      bool bgra = (key.bgra_mask >> loc) & 1;
      s += "  vec4 v" + std::to_string(v) + " = a" + std::to_string(loc) + (bgra ? ".zyxw" : "") + ";\n";
    }
    for (uint32_t r : temps) s += "  vec4 r" + std::to_string(r) + " = vec4(0.0);\n";
    s += "  ivec4 a0 = ivec4(0);\n";
    s += "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0), oD0 = vec4(1.0), oD1 = vec4(0.0), oFog = vec4(1.0), oPts = vec4(1.0);\n";
    s += "  vec4 oT[8];\n  for (int i = 0; i < 8; i++) oT[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
    s += body;
    s += "  v_c0 = clamp(oD0, 0.0, 1.0);\n  v_c1 = clamp(oD1, 0.0, 1.0);\n";
    s += "  for (int i = 0; i < 8; i++) v_t[i] = oT[i];\n";
    s += key.vfog == 1 ? "  v_fog = oPos.w;\n" : "  v_fog = oFog.x;\n";
    if (key.points) s += "  gl_PointSize = oPts.x;\n";
    s += "  gl_Position = vec4(oPos.x + oPos.w / u_vp.z, -oPos.y + oPos.w / u_vp.w, 2.0 * oPos.z - oPos.w, oPos.w);\n";
    s += "}\n";
    return s;
  }
};

}  // namespace

std::string sm_vertex_glsl(const std::vector<uint32_t>& tokens, const ProgKey& k, VsInfo* info, std::string* err) {
  Translator tr(tokens, k);
  if (!tr.run(info)) {
    *err = tr.err;
    return "";
  }
  return tr.glsl(*info);
}

}  // namespace gles
