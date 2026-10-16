// kernel32.dll: process, memory, threads, synchronisation, files, time, locale and module APIs.
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "../core/objects.h"

using namespace rt;

namespace {

const char* kDll = "kernel32.dll";

// Fake module handles for LoadLibrary/GetModuleHandle/GetProcAddress.
struct Module {
  const char* name;
  uint32_t handle;
};
const Module kModules[] = {{"kernel32.dll", 0x7c800000}, {"user32.dll", 0x77d40000}, {"gdi32.dll", 0x77f10000},
                           {"advapi32.dll", 0x77dd0000}, {"winmm.dll", 0x76b40000}, {"d3d9.dll", 0x4fdf0000},
                           {"dsound.dll", 0x73f10000},   {"dinput8.dll", 0x4c8b0000}, {"binkw32.dll", 0x10000000}};

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

uint32_t module_handle(std::string name) {
  name = lower(name);
  if (name.find('.') == std::string::npos) name += ".dll";
  size_t slash = name.find_last_of("\\/");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  for (auto& m : kModules)
    if (name == m.name) return m.handle;
  return 0;
}

const char* module_name(uint32_t h) {
  for (auto& m : kModules)
    if (m.handle == h) return m.name;
  return nullptr;
}

std::string exe_path() { return std::string(kGameDir) + "Shrek 2.exe"; }

// Windows-1252 <-> UTF-16.
const uint16_t k1252High[32] = {0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
                                0x2039, 0x0152, 0x8d, 0x017d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
                                0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d, 0x017e, 0x0178};
uint16_t cp1252_to_u16(uint8_t b) { return b >= 0x80 && b < 0xa0 ? k1252High[b - 0x80] : b; }
uint8_t u16_to_cp1252(uint16_t w, bool* lossy) {
  if (w < 0x80 || (w >= 0xa0 && w < 0x100)) return (uint8_t)w;
  for (int i = 0; i < 32; i++)
    if (k1252High[i] == w) return (uint8_t)(0x80 + i);
  if (lossy) *lossy = true;
  return '?';
}

const auto g_start = std::chrono::steady_clock::now();
uint64_t micros() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
}

// Critical sections are keyed by their guest address.
std::mutex g_cs_mutex;
std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_cs;
std::recursive_mutex* cs_get(uint32_t addr) {
  std::lock_guard<std::mutex> lock(g_cs_mutex);
  auto& p = g_cs[addr];
  if (!p) p = std::make_unique<std::recursive_mutex>();
  return p.get();
}

// TLS slots live in each TEB at +0xE10 (64 slots).
std::mutex g_tls_mutex;
uint64_t g_tls_used = 0;

uint32_t g_env_a, g_env_w, g_cmdline;

}  // namespace

// ---- Process and modules -------------------------------------------------------------------

HLE_STDCALL(kDll, GetCurrentProcessId, 0) { return 0x10; }
HLE_STDCALL(kDll, GetCurrentProcess, 0) { return kCurrentProcess; }
HLE_STDCALL(kDll, GetCurrentThreadId, 0) { return current_thread() ? current_thread()->id : 0; }
HLE_STDCALL(kDll, ExitProcess, 1) { process_exit(arg(c, 0)); }
HLE_STDCALL(kDll, TerminateProcess, 2) { process_exit(arg(c, 1)); }

HLE_STDCALL(kDll, GetCommandLineA, 0) {
  if (!g_cmdline) {
    std::string cl = "\"" + exe_path() + "\"";
    if (const char* extra = getenv("SHREK2_ARGS")) cl += std::string(" ") + extra;
    g_cmdline = sys_alloc((uint32_t)cl.size() + 1);
    write_cstr(g_cmdline, cl);
  }
  return g_cmdline;
}

HLE_STDCALL(kDll, GetStartupInfoA, 1) {
  uint32_t p = arg(c, 0);
  memset(host(p), 0, 68);
  st32(p, 68);
  return 0;
}

HLE_STDCALL(kDll, GetEnvironmentStrings, 0) {
  if (!g_env_a) {
    static const char env[] = "SystemRoot=C:\\WINDOWS\0windir=C:\\WINDOWS\0\0";
    g_env_a = sys_alloc(sizeof env);
    memcpy(host(g_env_a), env, sizeof env);
  }
  return g_env_a;
}

HLE_STDCALL(kDll, GetEnvironmentStringsW, 0) {
  if (!g_env_w) {
    static const char env[] = "SystemRoot=C:\\WINDOWS\0windir=C:\\WINDOWS\0\0";
    g_env_w = sys_alloc(sizeof env * 2);
    for (size_t i = 0; i < sizeof env; i++) st16(g_env_w + (uint32_t)i * 2, (uint8_t)env[i]);
  }
  return g_env_w;
}

HLE_STDCALL(kDll, FreeEnvironmentStringsA, 1) { return 1; }
HLE_STDCALL(kDll, FreeEnvironmentStringsW, 1) { return 1; }

HLE_STDCALL(kDll, GetModuleHandleA, 1) {
  uint32_t name = arg(c, 0);
  if (!name) return kImageBase;
  std::string n = lower(read_cstr(name));
  if (n.find("shrek 2") != std::string::npos) return kImageBase;
  uint32_t h = module_handle(n);
  if (!h) set_last_error(ERROR_MOD_NOT_FOUND);
  return h;
}

// D3DX loads d3d9.dll and looks up DebugSetMute on every shader and effect it creates: log each
// distinct lookup once.
static bool first_time(const std::string& what) {
  static std::mutex m;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> lock(m);
  return seen.insert(what).second;
}

HLE_STDCALL(kDll, LoadLibraryA, 1) {
  std::string n = read_cstr(arg(c, 0));
  uint32_t h = module_handle(n);
  if (first_time("load " + lower(n)) || trace_enabled("hle")) log(Log::Info, "LoadLibraryA(\"%s\") -> %08x", n.c_str(), h);
  if (!h) set_last_error(ERROR_MOD_NOT_FOUND);
  return h;
}

HLE_STDCALL(kDll, GetProcAddress, 2) {
  uint32_t mod = arg(c, 0), np = arg(c, 1);
  std::string name = np < 0x10000 ? "#" + std::to_string(np) : read_cstr(np);
  const char* dll = module_name(mod);
  HleFn fn = dll ? hle_find(dll, name) : nullptr;
  if (!fn) {
    if (first_time(std::string(dll ? dll : "?") + "!" + name) || trace_enabled("hle"))
      log(Log::Info, "GetProcAddress(%s, %s) -> not provided", dll ? dll : "?", name.c_str());
    set_last_error(ERROR_PROC_NOT_FOUND);
    return 0;
  }
  return make_thunk(fn, std::string(dll) + "!" + name);
}

HLE_STDCALL(kDll, GetModuleFileNameA, 3) {
  uint32_t buf = arg(c, 1), size = arg(c, 2);
  std::string p = exe_path();
  if (size == 0) return 0;
  uint32_t n = std::min<uint32_t>((uint32_t)p.size(), size - 1);
  memcpy(host(buf), p.data(), n);
  st8(buf + n, 0);
  return n;
}

HLE_STDCALL(kDll, GetVersionExA, 1) {
  uint32_t p = arg(c, 0);
  uint32_t size = ld32(p);
  st32(p + 4, 5);     // major (Windows XP)
  st32(p + 8, 1);     // minor
  st32(p + 12, 2600); // build
  st32(p + 16, 2);    // VER_PLATFORM_WIN32_NT
  write_cstr(p + 20, "Service Pack 2");
  if (size >= 156) {
    st16(p + 148, 2);  // wServicePackMajor
    st16(p + 150, 0);
  }
  return 1;
}

HLE_STDCALL(kDll, GetSystemInfo, 1) {
  uint32_t p = arg(c, 0);
  memset(host(p), 0, 36);
  st32(p + 4, kPage);
  st32(p + 8, 0x10000);
  st32(p + 12, 0x7ffeffff);
  st32(p + 16, 1);    // active processor mask
  st32(p + 20, 1);    // number of processors
  st32(p + 24, 586);  // PROCESSOR_INTEL_PENTIUM
  st32(p + 28, 0x10000);
  st16(p + 32, 6);    // level
  st16(p + 34, 0x0502);
  return 0;
}

HLE_STDCALL(kDll, IsProcessorFeaturePresent, 1) {
  switch (arg(c, 0)) {
    case 2:   // PF_COMPARE_EXCHANGE_DOUBLE
    case 8:   // PF_RDTSC_INSTRUCTION_AVAILABLE
      return 1;
    default:  // no MMX/SSE/SSE2/3DNow!: libraries take their x87 paths
      return 0;
  }
}

HLE_STDCALL(kDll, SetUnhandledExceptionFilter, 1) { return 0; }
HLE_STDCALL(kDll, UnhandledExceptionFilter, 1) { return 0; /* EXCEPTION_CONTINUE_SEARCH */ }

HLE_STDCALL(kDll, RaiseException, 4) {
  uint32_t code = arg(c, 0), n = arg(c, 2), args = arg(c, 3);
  std::string detail;
  if (code == 0xe06d7363 && n >= 3) {
    // MSVC C++ exception: args[2] = ThrowInfo; ThrowInfo->pCatchableTypeArray->types[0]->pType->name
    uint32_t info = ld32(args + 8);
    uint32_t cta = info ? ld32(info + 12) : 0;
    uint32_t ct = cta ? ld32(cta + 4) : 0;
    uint32_t td = ct ? ld32(ct + 4) : 0;
    detail = td ? " C++ type " + read_cstr(td + 8) : "";
  }
  fatal("RaiseException(code=%08x)%s", code, detail.c_str());
}

HLE_STDCALL(kDll, RtlUnwind, 4) { fatal("RtlUnwind called (SEH unwinding is not supported)"); }

// ---- Errors --------------------------------------------------------------------------------

HLE_STDCALL(kDll, GetLastError, 0) { return last_error(); }
HLE_STDCALL(kDll, SetLastError, 1) {
  set_last_error(arg(c, 0));
  return 0;
}

// ---- Memory --------------------------------------------------------------------------------

HLE_STDCALL(kDll, GetProcessHeap, 0) { return process_heap(); }
HLE_STDCALL(kDll, HeapCreate, 3) { return heap_create(arg(c, 1), arg(c, 2)); }
HLE_STDCALL(kDll, HeapDestroy, 1) { return heap_destroy(arg(c, 0)); }
HLE_STDCALL(kDll, HeapAlloc, 3) {
  uint32_t p = heap_alloc(arg(c, 0), arg(c, 2), (arg(c, 1) & 8) != 0);
  if (!p) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
  return p;
}
HLE_STDCALL(kDll, HeapFree, 3) { return heap_free(arg(c, 0), arg(c, 2)); }
HLE_STDCALL(kDll, HeapSize, 3) { return heap_size(arg(c, 0), arg(c, 2)); }
HLE_STDCALL(kDll, HeapReAlloc, 4) {
  uint32_t heap = arg(c, 0), flags = arg(c, 1), p = arg(c, 2), size = arg(c, 3);
  if (flags & 0x10) {  // HEAP_REALLOC_IN_PLACE_ONLY
    if (p && size <= heap_size(heap, p)) {
      st32(p - 16, size);
      return p;
    }
    return 0;
  }
  return heap_realloc(heap, p, size, (flags & 8) != 0);
}

HLE_STDCALL(kDll, VirtualAlloc, 4) {
  uint32_t addr = arg(c, 0), size = arg(c, 1), type = arg(c, 2);
  const uint32_t MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000;
  uint32_t base = addr & ~(kPage - 1);
  if ((type & MEM_RESERVE) || !addr) {
    base = vm_reserve(addr, size + (addr & (kPage - 1)));
    if (!base) {
      set_last_error(ERROR_NOT_ENOUGH_MEMORY);
      return 0;
    }
    if (addr) base = addr & ~(kPage - 1);
  }
  if (type & MEM_COMMIT) {
    if (!vm_commit(base, size + (addr & (kPage - 1)))) {
      set_last_error(ERROR_INVALID_PARAMETER);
      return 0;
    }
  }
  return base;
}

HLE_STDCALL(kDll, VirtualFree, 3) {
  uint32_t addr = arg(c, 0), size = arg(c, 1), type = arg(c, 2);
  if (type & 0x8000) return vm_release(addr);
  if (type & 0x4000) return vm_decommit(addr, size);
  return 0;
}

HLE_STDCALL(kDll, VirtualProtect, 4) {
  if (arg(c, 3)) st32(arg(c, 3), 0x04);  // PAGE_READWRITE
  return 1;
}

HLE_STDCALL(kDll, VirtualQuery, 3) {
  uint32_t addr = arg(c, 0), mbi = arg(c, 1);
  memset(host(mbi), 0, 28);
  VmRegion r;
  if (vm_query(addr, &r)) {
    st32(mbi + 0, r.base);
    st32(mbi + 4, r.alloc_base);
    st32(mbi + 8, 0x04);
    st32(mbi + 12, r.size);
    st32(mbi + 16, r.committed ? 0x1000 : 0x2000);
    st32(mbi + 20, r.committed ? 0x04 : 0);
    st32(mbi + 24, 0x20000);  // MEM_PRIVATE
  } else if (addr >= kImageBase && addr < kImageBase + 0x275000) {
    st32(mbi + 0, addr & ~(kPage - 1));
    st32(mbi + 4, kImageBase);
    st32(mbi + 8, 0x80);
    st32(mbi + 12, kPage);
    st32(mbi + 16, 0x1000);
    st32(mbi + 20, 0x40);
    st32(mbi + 24, 0x1000000);  // MEM_IMAGE
  } else {
    st32(mbi + 0, addr & ~(kPage - 1));
    st32(mbi + 12, kPage);
    st32(mbi + 16, 0x10000);  // MEM_FREE
    st32(mbi + 20, 0x01);
  }
  return 28;
}

static uint32_t bad_ptr(uint32_t p, uint32_t n) { return (p < kNullGuardEnd || (uint64_t)p + n > kGuestTop) ? 1 : 0; }
HLE_STDCALL(kDll, IsBadReadPtr, 2) { return bad_ptr(arg(c, 0), arg(c, 1)); }
HLE_STDCALL(kDll, IsBadWritePtr, 2) { return bad_ptr(arg(c, 0), arg(c, 1)); }
HLE_STDCALL(kDll, IsBadCodePtr, 1) { return bad_ptr(arg(c, 0), 1); }

// ---- Threads, TLS and synchronisation ------------------------------------------------------

HLE_STDCALL(kDll, TlsAlloc, 0) {
  std::lock_guard<std::mutex> lock(g_tls_mutex);
  for (uint32_t i = 0; i < 64; i++)
    if (!(g_tls_used & (1ull << i))) {
      g_tls_used |= 1ull << i;
      return i;
    }
  return 0xffffffffu;  // TLS_OUT_OF_INDEXES
}
HLE_STDCALL(kDll, TlsFree, 1) {
  std::lock_guard<std::mutex> lock(g_tls_mutex);
  g_tls_used &= ~(1ull << (arg(c, 0) & 63));
  return 1;
}
HLE_STDCALL(kDll, TlsGetValue, 1) {
  set_last_error(0);
  return ld32(current_thread()->teb + 0xe10 + 4 * (arg(c, 0) & 63));
}
HLE_STDCALL(kDll, TlsSetValue, 2) {
  st32(current_thread()->teb + 0xe10 + 4 * (arg(c, 0) & 63), arg(c, 1));
  return 1;
}

HLE_STDCALL(kDll, InitializeCriticalSection, 1) {
  memset(host(arg(c, 0)), 0, 24);
  st32(arg(c, 0) + 4, 0xffffffffu);  // LockCount = -1
  cs_get(arg(c, 0));
  return 0;
}
HLE_STDCALL(kDll, InitializeCriticalSectionAndSpinCount, 2) {
  memset(host(arg(c, 0)), 0, 24);
  st32(arg(c, 0) + 4, 0xffffffffu);
  cs_get(arg(c, 0));
  return 1;
}
HLE_STDCALL(kDll, DeleteCriticalSection, 1) {
  std::lock_guard<std::mutex> lock(g_cs_mutex);
  g_cs.erase(arg(c, 0));
  return 0;
}
HLE_STDCALL(kDll, EnterCriticalSection, 1) {
  cs_get(arg(c, 0))->lock();
  st32(arg(c, 0) + 12, current_thread()->id);  // OwningThread (informational)
  return 0;
}
HLE_STDCALL(kDll, LeaveCriticalSection, 1) {
  cs_get(arg(c, 0))->unlock();
  return 0;
}

HLE_STDCALL(kDll, InterlockedExchange, 2) { return __atomic_exchange_n((uint32_t*)host(arg(c, 0)), arg(c, 1), __ATOMIC_SEQ_CST); }

HLE_STDCALL(kDll, CreateMutexA, 3) {
  auto m = std::make_shared<MutexObject>();
  m->name = arg(c, 2) ? read_cstr(arg(c, 2)) : "";
  if (arg(c, 1)) m->wait(0);  // bInitialOwner
  set_last_error(0);          // never ERROR_ALREADY_EXISTS: one instance per page
  return handle_new(m);
}

HLE_STDCALL(kDll, ReleaseMutex, 1) {
  auto m = handle_as<MutexObject>(arg(c, 0));
  return m && m->release();
}

HLE_STDCALL(kDll, WaitForSingleObject, 2) {
  auto obj = handle_get(arg(c, 0));
  if (!obj) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0xffffffffu;  // WAIT_FAILED
  }
  return obj->wait(arg(c, 1)) ? 0 : 0x102;  // WAIT_OBJECT_0 / WAIT_TIMEOUT
}

HLE_STDCALL(kDll, CloseHandle, 1) {
  uint32_t h = arg(c, 0);
  if (h == kCurrentProcess || h == kCurrentThread) return 1;
  if (!handle_close(h)) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  return 1;
}

HLE_STDCALL(kDll, CreateThread, 6) {
  uint32_t stack = arg(c, 1), start = arg(c, 2), param = arg(c, 3), flags = arg(c, 4), tid = arg(c, 5);
  auto obj = std::make_shared<ThreadObject>();
  obj->thread = create_guest_thread(stack ? stack : 0x100000);
  obj->start = start;
  obj->param = param;
  obj->suspended = (flags & 4) != 0;  // CREATE_SUSPENDED
  if (tid) st32(tid, obj->thread->id);
  uint32_t h = handle_new(obj);
  obj->thread->handle = h;
  log(Log::Info, "CreateThread(start=%08x, param=%08x)%s -> thread %u", start, param, obj->suspended ? " suspended" : "", obj->thread->id);
  if (!obj->suspended) run_on_new_host_thread(obj->thread, start, param);
  return h;
}

HLE_STDCALL(kDll, ResumeThread, 1) {
  auto t = handle_as<ThreadObject>(arg(c, 0));
  if (!t) return 0xffffffffu;
  if (t->suspended) {
    t->suspended = false;
    run_on_new_host_thread(t->thread, t->start, t->param);
    return 1;
  }
  return 0;
}

HLE_STDCALL(kDll, SetThreadPriority, 2) { return 1; }

HLE_STDCALL(kDll, Sleep, 1) {
  uint32_t ms = arg(c, 0);
  if (ms > 2000 && ms != 0xffffffffu) log(Log::Warn, "Sleep(%u ms) from %08x", ms, ld32(c->esp));
  if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  else std::this_thread::yield();
  return 0;
}

// ---- Time ----------------------------------------------------------------------------------

HLE_STDCALL(kDll, GetTickCount, 0) { return (uint32_t)(micros() / 1000); }
HLE_STDCALL(kDll, QueryPerformanceFrequency, 1) {
  st64(arg(c, 0), 1000000ull);
  return 1;
}
HLE_STDCALL(kDll, QueryPerformanceCounter, 1) {
  st64(arg(c, 0), micros());
  return 1;
}
HLE_STDCALL(kDll, GetSystemTimeAsFileTime, 1) {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  uint64_t ft = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now).count() * 10 + 116444736000000000ull;
  st64(arg(c, 0), ft);
  return 0;
}

// ---- Locale and strings --------------------------------------------------------------------

HLE_STDCALL(kDll, GetACP, 0) { return 1252; }
HLE_STDCALL(kDll, GetOEMCP, 0) { return 437; }
HLE_STDCALL(kDll, IsValidCodePage, 1) { uint32_t cp = arg(c, 0); return cp == 1252 || cp == 437 || cp == 65001; }
HLE_STDCALL(kDll, GetUserDefaultLCID, 0) { return 0x0409; }
HLE_STDCALL(kDll, IsValidLocale, 2) { return arg(c, 0) == 0x0409; }
HLE_STDCALL(kDll, EnumSystemLocalesA, 2) {
  // The CRT only enumerates when setlocale() is given a language name.
  uint32_t buf = sys_alloc(16);
  write_cstr(buf, "00000409");
  guest_call(c, arg(c, 0), {buf});
  return 1;
}

HLE_STDCALL(kDll, GetCPInfo, 2) {
  uint32_t p = arg(c, 1);
  memset(host(p), 0, 20);
  st32(p, 1);       // MaxCharSize
  st8(p + 4, '?');  // DefaultChar
  return 1;
}

static std::string locale_info(uint32_t type) {
  switch (type & 0xffff) {
    case 0x1004: return "1252";     // LOCALE_IDEFAULTANSICODEPAGE
    case 0x000b: return "437";      // LOCALE_IDEFAULTCODEPAGE
    case 0x0001: return "0409";     // LOCALE_ILANGUAGE
    case 0x0003: return "ENU";      // LOCALE_SABBREVLANGNAME
    case 0x0007: return "USA";      // LOCALE_SABBREVCTRYNAME
    case 0x1001: return "English";  // LOCALE_SENGLANGUAGE
    case 0x1002: return "United States";
    case 0x0005: return "1";        // LOCALE_ICOUNTRY
    case 0x000e: return ".";        // LOCALE_SDECIMAL
    case 0x000f: return ",";        // LOCALE_STHOUSAND
    case 0x0010: return "3;0";      // LOCALE_SGROUPING
    case 0x0014: return "$";
    case 0x0015: return ".";
    case 0x0016: return ",";
    case 0x0017: return "3;0";
    case 0x0018: return "2";
    case 0x0019: return "2";
    case 0x001a: return "0";
    case 0x001b: return "1";
    case 0x001c: return "0";
    default: return "";
  }
}

HLE_STDCALL(kDll, GetLocaleInfoA, 4) {
  std::string v = locale_info(arg(c, 1));
  uint32_t buf = arg(c, 2), n = arg(c, 3);
  if (n == 0) return (uint32_t)v.size() + 1;
  if (n < v.size() + 1) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  write_cstr(buf, v);
  return (uint32_t)v.size() + 1;
}

HLE_STDCALL(kDll, GetLocaleInfoW, 4) {
  std::string v = locale_info(arg(c, 1));
  uint32_t buf = arg(c, 2), n = arg(c, 3);
  if (n == 0) return (uint32_t)v.size() + 1;
  if (n < v.size() + 1) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  write_wstr(buf, std::u16string(v.begin(), v.end()));
  return (uint32_t)v.size() + 1;
}

HLE_STDCALL(kDll, MultiByteToWideChar, 6) {
  uint32_t src = arg(c, 2), dst = arg(c, 4), dstlen = arg(c, 5);
  int32_t len = (int32_t)arg(c, 3);
  uint32_t n = len < 0 ? (uint32_t)read_cstr(src).size() + 1 : (uint32_t)len;
  if (dstlen == 0) return n;
  if (dstlen < n) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  for (uint32_t i = 0; i < n; i++) st16(dst + 2 * i, cp1252_to_u16(ld8(src + i)));
  return n;
}

HLE_STDCALL(kDll, WideCharToMultiByte, 8) {
  uint32_t src = arg(c, 2), dst = arg(c, 4), dstlen = arg(c, 5), used_default = arg(c, 7);
  int32_t len = (int32_t)arg(c, 3);
  uint32_t n = len < 0 ? (uint32_t)read_wstr(src).size() + 1 : (uint32_t)len;
  if (dstlen == 0) return n;
  if (dstlen < n) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  bool lossy = false;
  for (uint32_t i = 0; i < n; i++) st8(dst + i, u16_to_cp1252((uint16_t)ld16(src + 2 * i), &lossy));
  if (used_default) st32(used_default, lossy);
  return n;
}

static uint16_t ctype1(uint16_t ch) {
  uint16_t t = 0;
  if (ch < 0x80 || (ch >= 0xa0 && ch < 0x100)) {
    int c8 = ch;
    if (isupper(c8) || (c8 >= 0xc0 && c8 <= 0xde && c8 != 0xd7)) t |= 0x001;  // C1_UPPER
    if (islower(c8) || (c8 >= 0xdf && c8 <= 0xff && c8 != 0xf7)) t |= 0x002;  // C1_LOWER
    if (c8 < 0x80 && isdigit(c8)) t |= 0x004;
    if (c8 < 0x80 && isspace(c8)) t |= 0x008;
    if (c8 < 0x80 && ispunct(c8)) t |= 0x010;
    if (c8 < 0x20 || c8 == 0x7f) t |= 0x020;
    if (c8 == ' ' || c8 == '\t') t |= 0x040;
    if (c8 < 0x80 && isxdigit(c8)) t |= 0x080;
    if (t & 3) t |= 0x100;  // C1_ALPHA
  }
  return t;
}

HLE_STDCALL(kDll, GetStringTypeW, 4) {
  uint32_t src = arg(c, 1), out = arg(c, 3);
  int32_t len = (int32_t)arg(c, 2);
  uint32_t n = len < 0 ? (uint32_t)read_wstr(src).size() + 1 : (uint32_t)len;
  for (uint32_t i = 0; i < n; i++) st16(out + 2 * i, arg(c, 0) == 1 ? ctype1((uint16_t)ld16(src + 2 * i)) : 0);
  return 1;
}

HLE_STDCALL(kDll, GetStringTypeA, 5) {
  uint32_t src = arg(c, 2), out = arg(c, 4);
  int32_t len = (int32_t)arg(c, 3);
  uint32_t n = len < 0 ? (uint32_t)read_cstr(src).size() + 1 : (uint32_t)len;
  for (uint32_t i = 0; i < n; i++) st16(out + 2 * i, arg(c, 1) == 1 ? ctype1(cp1252_to_u16(ld8(src + i))) : 0);
  return 1;
}

static uint16_t map_case(uint16_t ch, uint32_t flags) {
  if ((flags & 0x100) && ch < 0x100 && (ctype1(ch) & 1)) return ch == 0xdf ? ch : (uint16_t)(ch + 0x20);  // LCMAP_LOWERCASE
  if ((flags & 0x200) && ch < 0x100 && (ctype1(ch) & 2) && ch != 0xdf && ch != 0xff) return (uint16_t)(ch - 0x20);
  return ch;
}

HLE_STDCALL(kDll, LCMapStringW, 6) {
  uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), dstlen = arg(c, 5);
  int32_t len = (int32_t)arg(c, 3);
  uint32_t n = len < 0 ? (uint32_t)read_wstr(src).size() + 1 : (uint32_t)len;
  if (dstlen == 0) return n;
  if (dstlen < n) return 0;
  for (uint32_t i = 0; i < n; i++) st16(dst + 2 * i, map_case((uint16_t)ld16(src + 2 * i), flags));
  return n;
}

HLE_STDCALL(kDll, LCMapStringA, 6) {
  uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), dstlen = arg(c, 5);
  int32_t len = (int32_t)arg(c, 3);
  uint32_t n = len < 0 ? (uint32_t)read_cstr(src).size() + 1 : (uint32_t)len;
  if (dstlen == 0) return n;
  if (dstlen < n) return 0;
  for (uint32_t i = 0; i < n; i++) st8(dst + i, u16_to_cp1252(map_case(cp1252_to_u16(ld8(src + i)), flags), nullptr));
  return n;
}

HLE_STDCALL(kDll, lstrcmpiA, 2) {
  std::string a = lower(read_cstr(arg(c, 0))), b = lower(read_cstr(arg(c, 1)));
  return a < b ? 0xffffffffu : a > b ? 1 : 0;
}

// ---- Console/std handles -------------------------------------------------------------------

HLE_STDCALL(kDll, GetStdHandle, 1) {
  static uint32_t handles[3];
  int32_t which = (int32_t)arg(c, 0);  // -10 stdin, -11 stdout, -12 stderr
  int i = -10 - which;
  if (i < 0 || i > 2) return kInvalidHandle;
  if (!handles[i]) {
    auto f = std::make_shared<FileObject>();
    f->is_console = true;
    f->fp = i == 0 ? nullptr : i == 1 ? stdout : stderr;
    handles[i] = handle_new(f);
  }
  return handles[i];
}
HLE_STDCALL(kDll, SetStdHandle, 2) { return 1; }
HLE_STDCALL(kDll, SetHandleCount, 1) { return arg(c, 0); }
