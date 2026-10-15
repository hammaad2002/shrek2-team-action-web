/* Minimal runtime for conformance tests: traps longjmp back to the caller. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <stdio.h>
#include <sys/mman.h>

#include "recomp.h"

static jmp_buf trap_jb;
static char trap_msg[512];

void rt_trap(Cpu* c, const char* msg) {
  (void)c;
  snprintf(trap_msg, sizeof trap_msg, "%s", msg);
  longjmp(trap_jb, 1);
}

static GuestFn lookup(uint32_t addr) {
  uint32_t lo = 0, hi = recomp_function_count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (recomp_functions[mid].addr < addr) lo = mid + 1;
    else hi = mid;
  }
  return lo < recomp_function_count && recomp_functions[lo].addr == addr ? recomp_functions[lo].fn : 0;
}

void rt_dispatch(Cpu* c, uint32_t target) {
  GuestFn fn = lookup(target);
  if (!fn) {
    snprintf(trap_msg, sizeof trap_msg, "dispatch to unknown 0x%08x", target);
    longjmp(trap_jb, 1);
  }
  fn(c);
}

void rt_hle(Cpu* c, uint32_t index) {
  snprintf(trap_msg, sizeof trap_msg, "hle import %u called", index);
  (void)c;
  longjmp(trap_jb, 1);
}

int test_init(void) {
  void* p = mmap((void*)GUEST_BASE, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
  return p == (void*)GUEST_BASE ? 0 : -1;
}

uint8_t* test_mem(void) { return (uint8_t*)GUEST_BASE; }
const char* test_trap_msg(void) { return trap_msg; }
uint32_t test_sizeof_cpu(void) { return (uint32_t)sizeof(Cpu); }

/* Returns 0 on normal return, 1 if the code trapped (message in test_trap_msg). */
int test_run(uint32_t addr, Cpu* c) {
  GuestFn fn = lookup(addr);
  if (!fn) {
    snprintf(trap_msg, sizeof trap_msg, "no function at 0x%08x", addr);
    return 2;
  }
  if (setjmp(trap_jb)) return 1;
  fn(c);
  return 0;
}
