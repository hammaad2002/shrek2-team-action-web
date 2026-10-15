/*
 * Interface between recompiled guest code and the runtime.
 * Both backends (generated C and generated WebAssembly) use this exact Cpu layout.
 */
#ifndef SHREK2WEB_RECOMP_H
#define SHREK2WEB_RECOMP_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest address space base: identity-mapped on the web (Emscripten data lives above the guest
 * window), a fixed 4 GiB-aligned mapping natively. */
#ifndef GUEST_BASE
#if defined(__EMSCRIPTEN__)
#define GUEST_BASE ((uintptr_t)0)
#else
#define GUEST_BASE ((uintptr_t)0x100000000ULL)
#endif
#endif

/* The generated code is compiled at -O1, where clang's inliner leaves small static helpers out of
 * line; memory accessors and x87 rounding must always be inlined (they dominate the profile). */
#define RT_INLINE static inline __attribute__((always_inline))

typedef struct Cpu {
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; /* 0x00 */
  uint32_t eflags;  /* 0x20: non-arithmetic EFLAGS bits (IF, AC, ID, ...) */
  uint32_t df;      /* 0x24 */
  uint32_t fs_base; /* 0x28 */
  uint32_t ftop;    /* 0x2c */
  uint32_t fcw;     /* 0x30 */
  uint32_t fsw;     /* 0x34: x87 status word without TOP (condition codes, exceptions) */
  uint32_t mxcsr;   /* 0x38 */
  uint32_t _pad0;   /* 0x3c */
  double fpr[8];    /* 0x40: x87 physical registers */
  uint64_t mm[8];   /* 0x80 */
  uint32_t xmm[32]; /* 0xc0: 8 x 4 lanes */
  /* runtime-private state follows */
  uint32_t thread_id;   /* 0x140 */
  uint32_t _pad1;
  void* rt;             /* per-thread runtime data */
} Cpu;

RT_INLINE uint8_t* gptr(uint32_t a) { return (uint8_t*)(GUEST_BASE + (uintptr_t)a); }
RT_INLINE uint32_t ld8(uint32_t a) { return *gptr(a); }
RT_INLINE uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, gptr(a), 2); return v; }
RT_INLINE uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, gptr(a), 4); return v; }
RT_INLINE uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, gptr(a), 8); return v; }
RT_INLINE float ldf32(uint32_t a) { float v; memcpy(&v, gptr(a), 4); return v; }
RT_INLINE double ldf64(uint32_t a) { double v; memcpy(&v, gptr(a), 8); return v; }
RT_INLINE void st8(uint32_t a, uint32_t v) { *gptr(a) = (uint8_t)v; }
RT_INLINE void st16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(gptr(a), &x, 2); }
RT_INLINE void st32(uint32_t a, uint32_t v) { memcpy(gptr(a), &v, 4); }
RT_INLINE void st64(uint32_t a, uint64_t v) { memcpy(gptr(a), &v, 8); }
RT_INLINE void stf32(uint32_t a, float v) { memcpy(gptr(a), &v, 4); }
RT_INLINE void stf64(uint32_t a, double v) { memcpy(gptr(a), &v, 8); }

/* Integer helpers with x86/WebAssembly semantics and no C undefined behaviour. */
RT_INLINE uint32_t rt_parity8(uint32_t x) { return !__builtin_parity(x & 0xff); }
RT_INLINE uint32_t sdiv32(uint32_t a, uint32_t b) { return (b == 0xffffffffu) ? 0u - a : (uint32_t)((int32_t)a / (int32_t)b); }
RT_INLINE uint32_t srem32(uint32_t a, uint32_t b) { return (b == 0xffffffffu) ? 0u : (uint32_t)((int32_t)a % (int32_t)b); }
RT_INLINE uint64_t sdiv64(uint64_t a, uint64_t b) { return (b == ~0ull) ? 0ull - a : (uint64_t)((int64_t)a / (int64_t)b); }
RT_INLINE uint64_t srem64(uint64_t a, uint64_t b) { return (b == ~0ull) ? 0ull : (uint64_t)((int64_t)a % (int64_t)b); }
RT_INLINE uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
RT_INLINE uint32_t urem32(uint32_t a, uint32_t b) { return b ? a % b : 0; }
RT_INLINE uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
RT_INLINE uint64_t urem64(uint64_t a, uint64_t b) { return b ? a % b : 0; }
RT_INLINE uint32_t rotl32(uint32_t x, uint32_t n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
RT_INLINE uint32_t rotr32(uint32_t x, uint32_t n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }
RT_INLINE uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
RT_INLINE uint32_t ctz32(uint32_t x) { return x ? (uint32_t)__builtin_ctz(x) : 32; }
RT_INLINE uint32_t f2i32sat(double v) {
  if (!(v == v)) return 0;
  if (v >= 2147483647.0) return 0x7fffffffu;
  if (v <= -2147483648.0) return 0x80000000u;
  return (uint32_t)(int32_t)v;
}
/* Round a double's significand to 24 bits (x87 precision control = single) without limiting
 * the exponent range: Veltkamp splitting with 2^29 + 1. */
RT_INLINE double rt_round24(double x) {
  double t = x * 536870913.0;
  double r = t - (t - x);
  return (r == r && t - t == 0.0) ? r : x; /* keep inf/NaN and overflowing inputs unchanged */
}
RT_INLINE float bits2f32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
RT_INLINE uint32_t f32bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
RT_INLINE double bits2f64(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }
RT_INLINE uint64_t f64bits(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }

/* Optional block tracing (build with -DRECOMP_TRACE): records the x86 address of every basic
 * block executed, for differential debugging against a reference emulator. */
#ifdef RECOMP_TRACE
void rt_trace_block(Cpu* c, uint32_t addr);
#define RT_BLOCK(a) rt_trace_block(c, (a))
#else
#define RT_BLOCK(a) ((void)0)
#endif

/* Runtime services (runtime/src). */
void rt_dispatch(Cpu* c, uint32_t target);
void rt_hle(Cpu* c, uint32_t import_index);
__attribute__((noreturn)) void rt_trap(Cpu* c, const char* msg);
void rt_cpuid(Cpu* c);
void rt_rep_movs(Cpu* c, uint32_t size, uint32_t df);
void rt_rep_stos(Cpu* c, uint32_t size, uint32_t df);
uint64_t rt_rcl(uint32_t a, uint32_t count, uint32_t cf, uint32_t bits);
uint64_t rt_rcr(uint32_t a, uint32_t count, uint32_t cf, uint32_t bits);

/* x87 helpers. */
double rt_f80_load(uint32_t addr);
void rt_f80_store(Cpu* c, uint32_t addr, double v);
uint32_t rt_fist16(double v, uint32_t fcw);
uint32_t rt_fist32(double v, uint32_t fcw);
uint64_t rt_fist64(double v, uint32_t fcw);
double rt_frndint(double v, uint32_t fcw);
uint32_t rt_fxam(double v);
double rt_sin(double v);
double rt_cos(double v);
double rt_tan(double v);
double rt_atan2(double y, double x);
double rt_log2(double v);
double rt_f2xm1(double v);
double rt_fscale(double a, double b);
void rt_fprem(Cpu* c, uint32_t ftop);
void rt_fprem1(Cpu* c, uint32_t ftop);
void rt_fnsave(Cpu* c, uint32_t addr);
void rt_fnstenv(Cpu* c, uint32_t addr);
void rt_frstor(Cpu* c, uint32_t addr);
void rt_fldenv(Cpu* c, uint32_t addr);

/* Function table emitted by the recompiler (C backend). */
typedef void (*GuestFn)(Cpu*);
typedef struct { uint32_t addr; GuestFn fn; } GuestFnEntry;
extern const GuestFnEntry recomp_functions[];
extern const uint32_t recomp_function_count;

#ifdef __cplusplus
}
#endif
#endif
