// Guest memory: the guest window, a bump allocator for runtime structures, a VirtualAlloc-style
// page allocator and dlmalloc-backed Heap* support.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>

#include "rt.h"

#if !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#endif

extern "C" {
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void* mspace_calloc(mspace msp, size_t n, size_t elem_size);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void mspace_free(mspace msp, void* mem);
}

namespace rt {

static std::mutex g_mem_mutex;

void memory_init() {
#if !defined(__EMSCRIPTEN__)
  // Reserve the full 4 GiB guest window plus a guard tail for unaligned accesses at the top.
  void* p = mmap((void*)GUEST_BASE, (1ull << 32) + 0x10000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
  if (p != (void*)GUEST_BASE) fatal("cannot map the guest address window at %p", (void*)GUEST_BASE);
  // Make guest NULL-pointer accesses fault natively (the web build cannot do this).
  mprotect((void*)GUEST_BASE, kNullGuardEnd, PROT_NONE);
#endif
}

std::string read_cstr(uint32_t addr, size_t max) {
  std::string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; i++) {
    uint8_t b = ld8(addr + (uint32_t)i);
    if (!b) break;
    s.push_back((char)b);
  }
  return s;
}

std::u16string read_wstr(uint32_t addr, size_t max) {
  std::u16string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; i++) {
    uint16_t ch = (uint16_t)ld16(addr + (uint32_t)i * 2);
    if (!ch) break;
    s.push_back((char16_t)ch);
  }
  return s;
}

void write_cstr(uint32_t addr, const std::string& s) {
  memcpy(host(addr), s.c_str(), s.size() + 1);
}

void write_wstr(uint32_t addr, const std::u16string& s) {
  for (size_t i = 0; i < s.size(); i++) st16(addr + (uint32_t)i * 2, s[i]);
  st16(addr + (uint32_t)s.size() * 2, 0);
}

// ---- System area ---------------------------------------------------------------------------

static uint32_t g_sys_next = kSysBase;

uint32_t sys_alloc(uint32_t size, uint32_t align) {
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  uint32_t a = (g_sys_next + align - 1) & ~(align - 1);
  if (a + size > kSysEnd) fatal("runtime system area exhausted");
  g_sys_next = a + size;
  memset(host(a), 0, size);
  return a;
}

// ---- Virtual memory ------------------------------------------------------------------------

struct Reservation {
  uint32_t size;
  std::vector<bool> committed;  // per page
};
static std::map<uint32_t, Reservation> g_reservations;

static std::map<uint32_t, Reservation>::iterator find_reservation(uint32_t addr) {
  auto it = g_reservations.upper_bound(addr);
  if (it == g_reservations.begin()) return g_reservations.end();
  --it;
  return addr < it->first + it->second.size ? it : g_reservations.end();
}

uint32_t vm_reserve(uint32_t addr, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  size = (size + kPage - 1) & ~(kPage - 1);
  if (size == 0) return 0;
  auto overlaps = [&](uint32_t base) {
    auto it = g_reservations.lower_bound(base);
    if (it != g_reservations.end() && it->first < base + size) return true;
    if (it != g_reservations.begin()) {
      --it;
      if (it->first + it->second.size > base) return true;
    }
    return false;
  };
  if (addr) {
    addr &= ~(kGranularity - 1);
    if (addr < kVirtualBase || addr + size > kVirtualEnd || overlaps(addr)) return 0;
  } else {
    // First fit, 64 KiB aligned.
    uint32_t cur = kVirtualBase;
    for (auto& [base, r] : g_reservations) {
      if (base >= cur + size) break;
      cur = std::max(cur, (base + r.size + kGranularity - 1) & ~(kGranularity - 1));
    }
    if (cur + size > kVirtualEnd) return 0;
    addr = cur;
  }
  g_reservations[addr] = Reservation{size, std::vector<bool>(size / kPage, false)};
  return addr;
}

bool vm_commit(uint32_t addr, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  auto it = find_reservation(addr);
  if (it == g_reservations.end()) return false;
  uint32_t first = (addr - it->first) / kPage;
  uint32_t last = (addr + size - 1 - it->first) / kPage;
  if (it->first + last * kPage >= it->first + it->second.size) return false;
  for (uint32_t p = first; p <= last; p++) {
    if (!it->second.committed[p]) {
      memset(host(it->first + p * kPage), 0, kPage);
      it->second.committed[p] = true;
    }
  }
  return true;
}

bool vm_decommit(uint32_t addr, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  auto it = find_reservation(addr);
  if (it == g_reservations.end()) return false;
  if (size == 0) size = it->second.size - (addr - it->first);
  uint32_t first = (addr - it->first) / kPage;
  uint32_t last = (addr + size - 1 - it->first) / kPage;
  for (uint32_t p = first; p <= last && p < it->second.committed.size(); p++) it->second.committed[p] = false;
  return true;
}

bool vm_release(uint32_t addr) {
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  return g_reservations.erase(addr) == 1;
}

bool vm_query(uint32_t addr, VmRegion* out) {
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  auto it = find_reservation(addr);
  if (it == g_reservations.end()) return false;
  uint32_t page = (addr - it->first) / kPage;
  bool c = it->second.committed[page];
  uint32_t start = page, end = page;
  while (start > 0 && it->second.committed[start - 1] == c) start--;
  while (end + 1 < it->second.committed.size() && it->second.committed[end + 1] == c) end++;
  *out = VmRegion{it->first + start * kPage, (end - start + 1) * kPage, it->first, c};
  return true;
}

// ---- Heaps ---------------------------------------------------------------------------------
// All Win32 heaps share one dlmalloc mspace over [kHeapBase, kHeapEnd). Each block carries a
// 16-byte header with the exact requested size, because HeapSize must return it exactly.

static mspace g_mspace;
static std::once_flag g_heap_once;
static uint32_t g_next_heap_id = 0x00a00000;
static uint32_t g_process_heap;

static void heap_init() {
  std::call_once(g_heap_once, [] {
    g_mspace = create_mspace_with_base(host(kHeapBase), kHeapEnd - kHeapBase, 1);
    if (!g_mspace) fatal("cannot create guest heap");
  });
}

uint32_t heap_create(uint32_t, uint32_t) {
  heap_init();
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  // Handles are just distinct cookies inside the system area (some code compares them).
  g_next_heap_id += 0x10;
  return g_next_heap_id;
}

bool heap_destroy(uint32_t) { return true; }

uint32_t process_heap() {
  if (!g_process_heap) g_process_heap = heap_create(0, 0);
  return g_process_heap;
}

uint32_t heap_alloc(uint32_t, uint32_t size, bool zero) {
  heap_init();
  void* p = zero ? mspace_calloc(g_mspace, 1, (size_t)size + 16) : mspace_malloc(g_mspace, (size_t)size + 16);
  if (!p) return 0;
  uint32_t g = guest_of(p);
  st32(g, size);
  st32(g + 4, 0x48454150);  // 'HEAP' cookie for sanity checks
  return g + 16;
}

uint32_t heap_size(uint32_t, uint32_t ptr) {
  if (!ptr) return 0xffffffffu;
  return ld32(ptr - 16);
}

bool heap_free(uint32_t, uint32_t ptr) {
  if (!ptr) return true;
  if (ld32(ptr - 12) != 0x48454150) {
    log(Log::Warn, "HeapFree of non-heap pointer 0x%08x", ptr);
    return false;
  }
  st32(ptr - 12, 0);
  mspace_free(g_mspace, host(ptr - 16));
  return true;
}

uint32_t heap_realloc(uint32_t heap, uint32_t ptr, uint32_t size, bool zero) {
  if (!ptr) return heap_alloc(heap, size, zero);
  uint32_t old = heap_size(heap, ptr);
  void* p = mspace_realloc(g_mspace, host(ptr - 16), (size_t)size + 16);
  if (!p) return 0;
  uint32_t g = guest_of(p);
  st32(g, size);
  if (zero && size > old) memset(host(g + 16 + old), 0, size - old);
  return g + 16;
}

}  // namespace rt
