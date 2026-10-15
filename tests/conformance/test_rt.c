/* Minimal runtime for conformance tests: traps longjmp back to the caller. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "recomp.h"

static sigjmp_buf trap_jb;
static char trap_msg[512];

void rt_trap(Cpu* c, const char* msg) {
  (void)c;
  snprintf(trap_msg, sizeof trap_msg, "%s", msg);
  siglongjmp(trap_jb, 1);
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
    siglongjmp(trap_jb, 1);
  }
  fn(c);
}

void rt_hle(Cpu* c, uint32_t index) {
  snprintf(trap_msg, sizeof trap_msg, "hle import %u called", index);
  (void)c;
  siglongjmp(trap_jb, 1);
}

int test_init(void) {
  void* p = mmap((void*)GUEST_BASE, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
  return p == (void*)GUEST_BASE ? 0 : -1;
}

uint8_t* test_mem(void) { return (uint8_t*)GUEST_BASE; }

#ifdef RECOMP_TRACE
static uint32_t trace_buf[1 << 20];
static uint32_t trace_len;
void rt_trace_block(Cpu* c, uint32_t addr) {
  (void)c;
  if (trace_len < (1u << 20)) trace_buf[trace_len++] = addr;
}
uint32_t* test_trace(uint32_t* len) { *len = trace_len; return trace_buf; }
void test_trace_reset(void) { trace_len = 0; }
#endif
const char* test_trap_msg(void) { return trap_msg; }
uint32_t test_sizeof_cpu(void) { return (uint32_t)sizeof(Cpu); }

/* Returns 0 on normal return, 1 if the code trapped (message in test_trap_msg). */
static void on_signal(int sig) {
  snprintf(trap_msg, sizeof trap_msg, sig == SIGALRM ? "timeout" : "host signal %d", sig);
  siglongjmp(trap_jb, sig == SIGALRM ? 3 : 4);
}

/* Returns 0 on normal return, 1 trap, 2 no such function, 3 timeout, 4 host fault. */
int test_run_ms(uint32_t addr, Cpu* c, int timeout_ms) {
  GuestFn fn = lookup(addr);
  if (!fn) {
    snprintf(trap_msg, sizeof trap_msg, "no function at 0x%08x", addr);
    return 2;
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGALRM, &sa, 0);
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGFPE, &sa, 0);
  struct itimerval it = {{0, 0}, {timeout_ms / 1000, (timeout_ms % 1000) * 1000}};
  int rc = sigsetjmp(trap_jb, 1);
  if (rc == 0) {
    setitimer(ITIMER_REAL, &it, 0);
    fn(c);
  }
  struct itimerval off = {{0, 0}, {0, 0}};
  setitimer(ITIMER_REAL, &off, 0);
  return rc;
}

int test_run(uint32_t addr, Cpu* c) { return test_run_ms(addr, c, 2000); }
