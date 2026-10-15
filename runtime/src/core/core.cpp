// Logging, dispatch, HLE thunks, PE image loading, guest threads and guest calls.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "rt.h"

namespace rt {

// ---- Logging -------------------------------------------------------------------------------

static std::mutex g_log_mutex;

bool trace_enabled(const char* channel) {
  static const char* env = getenv("SHREK2_TRACE");
  if (!env) return false;
  return strstr(env, channel) != nullptr || strstr(env, "all") != nullptr;
}

void log(Log level, const char* fmt, ...) {
  static const char* names[] = {"error", "warn", "info", "hle", "debug"};
  if (level == Log::Debug && !trace_enabled("debug")) return;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  GuestThread* t = current_thread();
  fprintf(stderr, "[%s t%u] ", names[(int)level], t ? t->id : 0);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static void backtrace(Cpu* c) {
  if (!c) return;
  fprintf(stderr, "  guest registers: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->eax,
          c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
  // Walk the EBP chain (frames without frame pointers are skipped).
  uint32_t ebp = c->ebp;
  for (int i = 0; i < 24 && ebp >= kNullGuardEnd && ebp < kGuestTop - 8; i++) {
    uint32_t ret = ld32(ebp + 4);
    fprintf(stderr, "  frame %2d: ebp=%08x ret=%08x\n", i, ebp, ret);
    uint32_t next = ld32(ebp);
    if (next <= ebp) break;
    ebp = next;
  }
  // Also dump the top of the stack: return addresses into .text are informative.
  fprintf(stderr, "  stack:");
  for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", ld32(c->esp + 4 * (uint32_t)i));
  fputc('\n', stderr);
}

void fatal(const char* fmt, ...) {
  {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    fprintf(stderr, "[fatal] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    GuestThread* t = current_thread();
    if (t) backtrace(&t->cpu);
  }
  fflush(stderr);
  abort();
}

}  // namespace rt

extern "C" void rt_trap(Cpu* c, const char* msg) {
  rt::fatal("guest trap: %s", msg);
  (void)c;
}

namespace rt {

// ---- Thunks and dispatch -------------------------------------------------------------------

struct Thunk {
  std::function<void(Cpu*)> fn;  // empty for imports that are not implemented
  std::string label;
};
static std::vector<Thunk> g_thunks;  // index = (addr - kThunkBase) / 4
static std::mutex g_thunk_mutex;
static const uint32_t kMaxThunks = (kThunkEnd - kThunkBase) / 4 - 16;

uint32_t make_thunk(std::function<void(Cpu*)> fn, const std::string& label) {
  std::lock_guard<std::mutex> lock(g_thunk_mutex);
  if (g_thunks.size() >= kMaxThunks) fatal("out of HLE thunks");
  g_thunks.push_back({fn, label});
  return kThunkBase + 4 * (uint32_t)(g_thunks.size() - 1);
}

const std::string& thunk_label(uint32_t addr) {
  static const std::string none = "?";
  uint32_t i = (addr - kThunkBase) / 4;
  return i < g_thunks.size() ? g_thunks[i].label : none;
}

static uint32_t g_text_lo, g_text_hi;
static std::vector<GuestFn> g_fn_table;  // direct-mapped over [g_text_lo, g_text_hi)

void dispatch_init() {
  g_text_lo = 0xffffffffu;
  g_text_hi = 0;
  for (uint32_t i = 0; i < recomp_function_count; i++) {
    g_text_lo = std::min(g_text_lo, recomp_functions[i].addr);
    g_text_hi = std::max(g_text_hi, recomp_functions[i].addr + 1);
  }
  g_fn_table.assign(g_text_hi - g_text_lo, nullptr);
  for (uint32_t i = 0; i < recomp_function_count; i++) g_fn_table[recomp_functions[i].addr - g_text_lo] = recomp_functions[i].fn;
  log(Log::Info, "dispatch: %u recompiled functions in [%08x, %08x)", recomp_function_count, g_text_lo, g_text_hi);
}

}  // namespace rt

extern "C" void rt_dispatch(Cpu* c, uint32_t target) {
  using namespace rt;
  uint32_t off = target - g_text_lo;
  if (off < g_fn_table.size()) {
    GuestFn fn = g_fn_table[off];
    if (fn) {
      fn(c);
      return;
    }
  } else if (target >= kThunkBase && target < kThunkEnd) {
    uint32_t i = (target - kThunkBase) / 4;
    if (i < g_thunks.size() && (target & 3) == 0) {
      if (!g_thunks[i].fn) fatal("unimplemented %s (called indirectly from %08x)", g_thunks[i].label.c_str(), ld32(c->esp));
      g_thunks[i].fn(c);
      return;
    }
  }
  fatal("dispatch to unknown guest address 0x%08x (no recompiled function or thunk)", target);
}

namespace rt {

// ---- HLE registry and import binding -------------------------------------------------------

static std::map<std::string, HleFn>& registry() {
  static std::map<std::string, HleFn> r;
  return r;
}

static std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}

void hle_register(const char* dll, const char* name, HleFn fn) { registry()[lower(dll) + "!" + name] = fn; }

HleFn hle_find(const std::string& dll, const std::string& name) {
  std::string d = lower(dll);
  if (d.find('.') == std::string::npos) d += ".dll";
  auto it = registry().find(d + "!" + name);
  return it == registry().end() ? nullptr : it->second;
}

struct Import {
  std::string dll, name;
  uint32_t slot;
  HleFn fn;
};
static std::vector<Import> g_imports;
static uint32_t g_entry;

uint32_t image_entry() { return g_entry; }

void load_image(const std::vector<uint8_t>& exe) {
  auto u16 = [&](size_t o) { return (uint32_t)(exe[o] | exe[o + 1] << 8); };
  auto u32 = [&](size_t o) { return (uint32_t)(exe[o] | exe[o + 1] << 8 | exe[o + 2] << 16 | (uint32_t)exe[o + 3] << 24); };
  uint32_t pe = u32(0x3c);
  if (u32(pe) != 0x4550) fatal("not a PE image");
  uint32_t nsec = u16(pe + 6), optsz = u16(pe + 20), opt = pe + 24;
  uint32_t base = u32(opt + 28), size = u32(opt + 56), hdrsz = u32(opt + 60);
  if (base != kImageBase) fatal("unexpected image base %08x", base);
  memcpy(host(base), exe.data(), std::min<size_t>(hdrsz, exe.size()));
  for (uint32_t i = 0; i < nsec; i++) {
    uint32_t so = opt + optsz + 40 * i;
    uint32_t vs = u32(so + 8), va = u32(so + 12), rs = u32(so + 16), ro = u32(so + 20);
    memset(host(base + va), 0, vs);
    uint32_t n = std::min(rs, vs ? vs : rs);
    if (ro + n > exe.size()) n = (uint32_t)exe.size() - ro;
    memcpy(host(base + va), exe.data() + ro, n);
  }
  (void)size;
  g_entry = base + u32(opt + 16);
  // Imports: fill each IAT slot with a thunk for the HLE implementation (or a trap).
  uint32_t imp = u32(opt + 104);
  for (uint32_t d = base + imp;; d += 20) {
    uint32_t name_rva = ld32(d + 12);
    if (!name_rva) break;
    std::string dll = lower(read_cstr(base + name_rva));
    uint32_t lookup = ld32(d) ? ld32(d) : ld32(d + 16);
    uint32_t iat = ld32(d + 16);
    for (uint32_t k = 0;; k++) {
      uint32_t t = ld32(base + lookup + 4 * k);
      if (!t) break;
      std::string name = (t & 0x80000000u) ? "#" + std::to_string(t & 0xffff) : read_cstr(base + t + 2);
      uint32_t slot = base + iat + 4 * k;
      HleFn fn = hle_find(dll, name);
      g_imports.push_back({dll, name, slot, fn});
    }
  }
  // Import indices must match the recompiler's order (pe.ts walks descriptors the same way).
  for (size_t i = 0; i < g_imports.size(); i++) {
    Import& im = g_imports[i];
    uint32_t thunk = make_thunk(im.fn ? std::function<void(Cpu*)>(im.fn) : std::function<void(Cpu*)>(), im.dll + "!" + im.name);
    st32(im.slot, thunk);
  }
  size_t missing = 0;
  for (auto& im : g_imports)
    if (!im.fn) missing++;
  log(Log::Info, "image loaded: entry %08x, %zu imports (%zu not implemented yet)", g_entry, g_imports.size(), missing);
}

}  // namespace rt

extern "C" void rt_hle(Cpu* c, uint32_t index) {
  using namespace rt;
  if (index >= g_imports.size()) fatal("bad import index %u", index);
  Import& im = g_imports[index];
  if (!im.fn) fatal("unimplemented import %s!%s (called from %08x)", im.dll.c_str(), im.name.c_str(), ld32(c->esp));
  if (trace_enabled("hle")) log(Log::Hle, "%s(%08x, %08x, %08x, %08x) from %08x", im.name.c_str(), arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), ld32(c->esp));
  im.fn(c);
}

namespace rt {

// ---- Threads -------------------------------------------------------------------------------

static thread_local GuestThread* t_current = nullptr;
static std::atomic<uint32_t> g_next_tid{0x100};
static uint32_t g_peb;

GuestThread* current_thread() { return t_current; }
void bind_host_thread(GuestThread* t) { t_current = t; }

uint32_t last_error() { return t_current ? ld32(t_current->teb + 0x34) : 0; }
void set_last_error(uint32_t e) {
  if (t_current) st32(t_current->teb + 0x34, e);
}

GuestThread* create_guest_thread(uint32_t stack_size) {
  if (!g_peb) {
    g_peb = sys_alloc(0x1000);
    st32(g_peb + 0x08, kImageBase);  // ImageBaseAddress
    st32(g_peb + 0x18, process_heap());
  }
  auto* t = new GuestThread();
  t->id = g_next_tid.fetch_add(4);
  stack_size = std::max<uint32_t>((stack_size + kGranularity - 1) & ~(kGranularity - 1), 0x40000);
  uint32_t stack = vm_reserve(0, stack_size);
  if (!stack || !vm_commit(stack, stack_size)) fatal("cannot allocate a %u-byte guest stack", stack_size);
  t->stack_limit = stack;
  t->stack_base = stack + stack_size;
  t->teb = sys_alloc(0x1000, 0x1000);
  st32(t->teb + 0x00, 0xffffffffu);  // SEH chain terminator
  st32(t->teb + 0x04, t->stack_base);
  st32(t->teb + 0x08, t->stack_limit);
  st32(t->teb + 0x18, t->teb);  // NT_TIB.Self
  st32(t->teb + 0x20, 0x10);    // ClientId.UniqueProcess
  st32(t->teb + 0x24, t->id);   // ClientId.UniqueThread
  st32(t->teb + 0x30, g_peb);
  Cpu& c = t->cpu;
  c.esp = t->stack_base - 16;
  c.fs_base = t->teb;
  c.fcw = 0x27f;  // Windows default: 53-bit precision, round to nearest, exceptions masked
  c.eflags = 0x202;
  c.thread_id = t->id;
  c.rt = t;
  return t;
}

uint32_t guest_call(Cpu* c, uint32_t addr, std::initializer_list<uint32_t> args) {
  uint32_t saved = c->esp;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c->esp -= 4;
    st32(c->esp, *it);
  }
  c->esp -= 4;
  st32(c->esp, kReturnSentinel);
  rt_dispatch(c, addr);
  c->esp = saved;  // correct for both stdcall and cdecl callees
  return c->eax;
}

void run_on_new_host_thread(GuestThread* t, uint32_t start, uint32_t param) {
  std::thread([t, start, param] {
    bind_host_thread(t);
    uint32_t code = guest_call(&t->cpu, start, {param});
    t->exit_code = code;
    t->finished.store(true);
    log(Log::Debug, "guest thread %u exited with %u", t->id, code);
  }).detach();
}

[[noreturn]] void process_exit(uint32_t code) {
  log(Log::Info, "process exit (%u)", code);
  fflush(stderr);
  fflush(stdout);
  _Exit((int)code);
}

}  // namespace rt
