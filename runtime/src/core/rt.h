// Core runtime interfaces shared by the HLE modules.
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "recomp.h"

namespace rt {

// ---------------------------------------------------------------------------------------------
// Guest address-space layout (the guest window is identity-mapped on the web, so everything the
// game can see must fit below kGuestTop).
constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kGranularity = 0x10000;
constexpr uint32_t kNullGuardEnd = 0x00010000;
constexpr uint32_t kSysBase = 0x00010000;  // PEB, TEBs, environment, misc runtime structures
constexpr uint32_t kSysEnd = 0x002F0000;
constexpr uint32_t kThunkBase = 0x002F0000;  // HLE entry points (imports and COM methods)
constexpr uint32_t kThunkEnd = 0x00300000;
constexpr uint32_t kComBase = 0x00300000;  // COM objects and vtables handed to the game
constexpr uint32_t kComEnd = 0x00400000;
constexpr uint32_t kImageBase = 0x00400000;
constexpr uint32_t kVirtualBase = 0x00700000;  // VirtualAlloc / thread stacks / file mappings
constexpr uint32_t kVirtualEnd = 0x18000000;
constexpr uint32_t kHeapBase = 0x18000000;  // Heap* APIs (dlmalloc mspace)
constexpr uint32_t kHeapEnd = 0x3FF00000;
constexpr uint32_t kGuestTop = 0x40000000;
constexpr uint32_t kReturnSentinel = 0x002FFFF0;  // inside the thunk region, never a real thunk

// ---------------------------------------------------------------------------------------------
// Logging and fatal errors.
enum class Log { Error, Warn, Info, Hle, Debug };
void log(Log level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool trace_enabled(const char* channel);

// ---------------------------------------------------------------------------------------------
// Guest memory.
void memory_init();
inline uint8_t* host(uint32_t guest) { return gptr(guest); }
inline uint32_t guest_of(const void* p) { return (uint32_t)((uintptr_t)p - GUEST_BASE); }
std::string read_cstr(uint32_t addr, size_t max = 65536);
std::u16string read_wstr(uint32_t addr, size_t max = 65536);
void write_cstr(uint32_t addr, const std::string& s);  // includes the terminator
void write_wstr(uint32_t addr, const std::u16string& s);

// Low system area bump allocator (never freed): PEB, TEBs, strings handed out by the runtime.
uint32_t sys_alloc(uint32_t size, uint32_t align = 16);

// VirtualAlloc-style page allocator.
uint32_t vm_reserve(uint32_t addr, uint32_t size);  // returns 0 on failure
bool vm_commit(uint32_t addr, uint32_t size);
bool vm_decommit(uint32_t addr, uint32_t size);
bool vm_release(uint32_t addr);
struct VmRegion { uint32_t base, size, alloc_base; bool committed; };
bool vm_query(uint32_t addr, VmRegion* out);

// Heap API backing (one mspace per HeapCreate; handles are opaque ids).
uint32_t heap_create(uint32_t initial, uint32_t max);
bool heap_destroy(uint32_t heap);
uint32_t heap_alloc(uint32_t heap, uint32_t size, bool zero);
uint32_t heap_realloc(uint32_t heap, uint32_t ptr, uint32_t size, bool zero);
bool heap_free(uint32_t heap, uint32_t ptr);
uint32_t heap_size(uint32_t heap, uint32_t ptr);
uint32_t process_heap();

// ---------------------------------------------------------------------------------------------
// Threads.
struct GuestThread {
  Cpu cpu{};
  uint32_t id = 0;
  uint32_t teb = 0;
  uint32_t stack_base = 0;   // highest address (exclusive)
  uint32_t stack_limit = 0;  // lowest committed address
  uint32_t handle = 0;       // kernel handle for this thread (waitable)
  std::atomic<bool> finished{false};
  uint32_t exit_code = 0x103;  // STILL_ACTIVE
  // Watchdog bookkeeping: the HLE function currently executing and a running call count.
  std::atomic<const char*> in_hle{nullptr};
  std::atomic<uint32_t> hle_ret{0};
  std::atomic<uint64_t> hle_calls{0};
  std::atomic<const char*> last_hle{nullptr};
};

// Called by the renderer for every presented frame; SHREK2_WATCHDOG=<seconds> reports what each
// guest thread is doing when no frame has been presented for that long.
void note_present();
void watchdog_start();

GuestThread* current_thread();
GuestThread* create_guest_thread(uint32_t stack_size);
void run_on_new_host_thread(GuestThread* t, uint32_t start, uint32_t param);
void bind_host_thread(GuestThread* t);
uint32_t last_error();
void set_last_error(uint32_t e);

// Call guest code at `addr` with stdcall/cdecl-agnostic argument handling; returns EAX.
uint32_t guest_call(Cpu* c, uint32_t addr, std::initializer_list<uint32_t> args);

// ---------------------------------------------------------------------------------------------
// Dispatch and HLE.
using HleFn = void (*)(Cpu* c);
struct HleExport {
  const char* dll;   // lower-case, e.g. "kernel32.dll"
  const char* name;  // e.g. "CreateFileA" or "#11"
  HleFn fn;
};
void hle_register(const char* dll, const char* name, HleFn fn);
HleFn hle_find(const std::string& dll, const std::string& name);
uint32_t make_thunk(std::function<void(Cpu*)> fn, const std::string& label);  // guest address dispatching to fn
const std::string& thunk_label(uint32_t addr);
void dispatch_init();
void load_image(const std::vector<uint8_t>& exe);  // maps sections and binds imports
uint32_t image_entry();
[[noreturn]] void process_exit(uint32_t code);

// Argument and return helpers for HLE functions (the return address is at [esp]).
inline uint32_t arg(Cpu* c, int i) { return ld32(c->esp + 4 + 4 * (uint32_t)i); }
inline void ret_stdcall(Cpu* c, uint32_t eax, int nargs) {
  c->eax = eax;
  c->esp += 4 + 4 * (uint32_t)nargs;
}
inline void ret_cdecl(Cpu* c, uint32_t eax) {
  c->eax = eax;
  c->esp += 4;
}

// Registers HLE functions at static-initialisation time.
struct HleRegistrar {
  HleRegistrar(const char* dll, const char* name, HleFn fn) { hle_register(dll, name, fn); }
};
#define RT_CAT2(a, b) a##b
#define RT_CAT(a, b) RT_CAT2(a, b)
// Defines `static uint32_t impl(Cpu* c)` registered as dll!name with stdcall argument count n.
#define HLE_STDCALL(dll, name, n)                                                   \
  static uint32_t hle_impl_##name(Cpu* c);                                         \
  static void hle_entry_##name(Cpu* c) { ret_stdcall(c, hle_impl_##name(c), n); } \
  static rt::HleRegistrar RT_CAT(hle_reg_, __LINE__)(dll, #name, hle_entry_##name); \
  static uint32_t hle_impl_##name(Cpu* c)
// Same, for exports whose names are not C identifiers (e.g. "_BinkOpen@8").
#define HLE_STDCALL_AS(dll, ident, export_name, n)                                    \
  static uint32_t hle_impl_##ident(Cpu* c);                                          \
  static void hle_entry_##ident(Cpu* c) { ret_stdcall(c, hle_impl_##ident(c), n); } \
  static rt::HleRegistrar RT_CAT(hle_reg_, __LINE__)(dll, export_name, hle_entry_##ident); \
  static uint32_t hle_impl_##ident(Cpu* c)

}  // namespace rt
