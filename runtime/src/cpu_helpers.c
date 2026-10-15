/*
 * CPU helpers called from recompiled code: x87 conversions and transcendental operations,
 * repeated string moves/stores, rotate-through-carry and CPUID.
 */
#include <math.h>
#include <string.h>

#include "recomp.h"

/* ---- Bulk string operations (byte-exact x86 semantics, including overlapping copies) ---- */

void rt_rep_movs(Cpu* c, uint32_t size, uint32_t df) {
  uint32_t n = c->ecx;
  if (n == 0) return;
  uint32_t src = c->esi, dst = c->edi;
  uint64_t bytes = (uint64_t)n * size;
  if (!df) {
    /* Forward copy: memmove is only equivalent when the regions don't overlap destructively. */
    if (dst <= src || (uint64_t)dst >= (uint64_t)src + bytes) {
      memmove(gptr(dst), gptr(src), (size_t)bytes);
    } else {
      for (uint64_t i = 0; i < bytes; i++) gptr(dst)[i] = gptr(src)[i];
    }
    c->esi = src + (uint32_t)bytes;
    c->edi = dst + (uint32_t)bytes;
  } else {
    for (uint32_t i = 0; i < n; i++) {
      memcpy(gptr(dst), gptr(src), size);
      src -= size;
      dst -= size;
    }
    c->esi = src;
    c->edi = dst;
  }
  c->ecx = 0;
}

void rt_rep_stos(Cpu* c, uint32_t size, uint32_t df) {
  uint32_t n = c->ecx;
  if (n == 0) return;
  uint32_t dst = c->edi;
  uint32_t v = c->eax;
  if (!df && size == 1) {
    memset(gptr(dst), (int)(v & 0xff), n);
    c->edi = dst + n;
  } else {
    int32_t step = df ? -(int32_t)size : (int32_t)size;
    for (uint32_t i = 0; i < n; i++) {
      memcpy(gptr(dst), &v, size);
      dst += (uint32_t)step;
    }
    c->edi = dst;
  }
  c->ecx = 0;
}

/* ---- Rotate through carry: returns result | (cf << 32) ---- */

uint64_t rt_rcl(uint32_t a, uint32_t count, uint32_t cf, uint32_t bits) {
  uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
  count &= 0x1f;
  if (bits < 32) count %= (bits + 1);
  a &= mask;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t out = (a >> (bits - 1)) & 1;
    a = ((a << 1) | cf) & mask;
    cf = out;
  }
  return (uint64_t)a | ((uint64_t)cf << 32);
}

uint64_t rt_rcr(uint32_t a, uint32_t count, uint32_t cf, uint32_t bits) {
  uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
  count &= 0x1f;
  if (bits < 32) count %= (bits + 1);
  a &= mask;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t out = a & 1;
    a = ((a >> 1) | (cf << (bits - 1))) & mask;
    cf = out;
  }
  return (uint64_t)a | ((uint64_t)cf << 32);
}

/* ---- CPUID: a conservative Pentium II-class CPU without MMX/SSE/3DNow!, so libraries such as
 * D3DX take their plain x87 code paths. ---- */

void rt_cpuid(Cpu* c) {
  uint32_t leaf = c->eax;
  if (leaf == 0) {
    c->eax = 1;
    memcpy(&c->ebx, "Genu", 4);
    memcpy(&c->edx, "ineI", 4);
    memcpy(&c->ecx, "ntel", 4);
  } else if (leaf == 1) {
    c->eax = 0x00000652; /* family 6, model 5, stepping 2 */
    c->ebx = 0;
    c->ecx = 0;
    c->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15); /* FPU, TSC, CX8, CMOV */
  } else if (leaf >= 0x80000000u) {
    c->eax = 0x80000000u; /* no extended leaves (no 3DNow!) */
    c->ebx = c->ecx = c->edx = 0;
  } else {
    c->eax = c->ebx = c->ecx = c->edx = 0;
  }
}

/* ---- x87 ---- */

static double round_rc(double v, uint32_t fcw) {
  switch ((fcw >> 10) & 3) {
    case 0: return nearbyint(v); /* default mode is round-to-nearest-even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
  }
}

double rt_frndint(double v, uint32_t fcw) {
  if (!isfinite(v)) return v;
  return round_rc(v, fcw);
}

uint32_t rt_fist16(double v, uint32_t fcw) {
  double r = round_rc(v, fcw);
  if (!(r >= -32768.0 && r <= 32767.0)) return 0x8000u;
  return (uint32_t)(uint16_t)(int16_t)r;
}

uint32_t rt_fist32(double v, uint32_t fcw) {
  double r = round_rc(v, fcw);
  if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0x80000000u;
  return (uint32_t)(int32_t)r;
}

uint64_t rt_fist64(double v, uint32_t fcw) {
  double r = round_rc(v, fcw);
  if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return 0x8000000000000000ull;
  return (uint64_t)(int64_t)r;
}

/* 80-bit extended <-> double. */
double rt_f80_load(uint32_t addr) {
  uint64_t mant;
  uint16_t se;
  memcpy(&mant, gptr(addr), 8);
  memcpy(&se, gptr(addr + 8), 2);
  int sign = se >> 15;
  int exp = se & 0x7fff;
  double v;
  if (exp == 0 && mant == 0) v = 0.0;
  else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
  else v = ldexp((double)mant, exp - 16383 - 63);
  return sign ? -v : v;
}

void rt_f80_store(Cpu* c, uint32_t addr, double v) {
  (void)c;
  uint64_t mant = 0;
  uint16_t se = signbit(v) ? 0x8000 : 0;
  if (isnan(v)) {
    se |= 0x7fff;
    mant = 0xc000000000000000ull;
  } else if (isinf(v)) {
    se |= 0x7fff;
    mant = 0x8000000000000000ull;
  } else if (v != 0.0) {
    int e;
    double m = frexp(fabs(v), &e); /* v = m * 2^e, m in [0.5, 1) */
    mant = (uint64_t)ldexp(m, 64);
    se |= (uint16_t)(e - 1 + 16383);
  }
  memcpy(gptr(addr), &mant, 8);
  memcpy(gptr(addr + 8), &se, 2);
}

uint32_t rt_fxam(double v) {
  uint32_t c1 = signbit(v) ? 1u << 9 : 0;
  if (isnan(v)) return c1 | (1u << 8);                       /* C0 */
  if (isinf(v)) return c1 | (1u << 10) | (1u << 8);          /* C2 C0 */
  if (v == 0.0) return c1 | (1u << 14);                      /* C3 */
  if (fpclassify(v) == FP_SUBNORMAL) return c1 | (1u << 14) | (1u << 10);
  return c1 | (1u << 10);                                    /* C2: normal */
}

/* fsin/fcos/fptan leave operands with |x| >= 2^63 unchanged (and set C2). */
#define X87_TRIG_LIMIT 9223372036854775808.0
double rt_sin(double v) { return fabs(v) < X87_TRIG_LIMIT ? sin(v) : v; }
double rt_cos(double v) { return fabs(v) < X87_TRIG_LIMIT ? cos(v) : v; }
double rt_tan(double v) { return fabs(v) < X87_TRIG_LIMIT ? tan(v) : v; }
double rt_atan2(double y, double x) { return atan2(y, x); }
double rt_log2(double v) { return log2(v); }
double rt_f2xm1(double v) { return exp2(v) - 1.0; }
double rt_fscale(double a, double b) { return ldexp(a, (int)trunc(fmax(fmin(b, 100000.0), -100000.0))); }

static void fprem_common(Cpu* c, uint32_t ftop, int ieee) {
  double* st0 = &c->fpr[ftop & 7];
  double st1 = c->fpr[(ftop + 1) & 7];
  double q = ieee ? nearbyint(*st0 / st1) : trunc(*st0 / st1);
  double r = ieee ? remainder(*st0, st1) : fmod(*st0, st1);
  long long qi = (long long)fabs(q);
  *st0 = r;
  /* C2 = 0 (complete), C0/C3/C1 = quotient bits 2,1,0. */
  uint32_t fsw = c->fsw & ~0x4700u;
  if (qi & 4) fsw |= 1u << 8;
  if (qi & 2) fsw |= 1u << 14;
  if (qi & 1) fsw |= 1u << 9;
  c->fsw = fsw;
}

void rt_fprem(Cpu* c, uint32_t ftop) { fprem_common(c, ftop, 0); }
void rt_fprem1(Cpu* c, uint32_t ftop) { fprem_common(c, ftop, 1); }

/* FNSTENV/FLDENV (28-byte protected-mode format) and FNSAVE/FRSTOR (+ 8 x 80-bit registers). */
void rt_fnstenv(Cpu* c, uint32_t addr) {
  uint32_t env[7] = {0};
  env[0] = c->fcw | 0xffff0000u;
  env[1] = ((c->fsw & ~0x3800u) | ((c->ftop & 7) << 11)) | 0xffff0000u;
  env[2] = 0xffffffffu; /* tag word: all registers reported empty (tags are not modelled) */
  memcpy(gptr(addr), env, sizeof env);
}

void rt_fldenv(Cpu* c, uint32_t addr) {
  uint32_t env[7];
  memcpy(env, gptr(addr), sizeof env);
  c->fcw = env[0] & 0xffff;
  c->fsw = env[1] & 0xffff & ~0x3800u;
  c->ftop = (env[1] >> 11) & 7;
}

void rt_fnsave(Cpu* c, uint32_t addr) {
  rt_fnstenv(c, addr);
  for (int i = 0; i < 8; i++) rt_f80_store(c, addr + 28 + i * 10, c->fpr[(c->ftop + i) & 7]);
  /* FNSAVE reinitialises the FPU. */
  c->fcw = 0x37f;
  c->fsw = 0;
  c->ftop = 0;
}

void rt_frstor(Cpu* c, uint32_t addr) {
  rt_fldenv(c, addr);
  for (int i = 0; i < 8; i++) c->fpr[(c->ftop + i) & 7] = rt_f80_load(addr + 28 + i * 10);
}
