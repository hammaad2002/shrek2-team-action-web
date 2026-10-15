// Player build: guest code is a separate WebAssembly module translated in the player's browser
// (recomp/src/translate.ts) and instantiated lazily in every worker that runs guest code. It
// shares this module's memory; guest functions are found through a u16 index over .text.
#include <emscripten.h>

#include <cstring>
#include <vector>

#include "../core/rt.h"

namespace {

uint8_t* g_game_bytes = nullptr;
uint32_t g_game_len = 0;
uint16_t* g_index = nullptr;
uint32_t g_text_lo = 0, g_text_size = 0;
std::vector<uint32_t> g_funcs;

}  // namespace

extern "C" {

void rt_dispatch_ext(Cpu* c, uint32_t target);
void rt_trap_id(Cpu* c, uint32_t id);

// The page hands over the translated module before callMain().
EMSCRIPTEN_KEEPALIVE uint8_t* web_game_alloc(uint32_t len) {
  g_game_bytes = (uint8_t*)malloc(len);
  g_game_len = len;
  return g_game_bytes;
}

// Function entry addresses in table order (the module's "shrek2.funcs" section).
EMSCRIPTEN_KEEPALIVE void web_game_register(const uint32_t* addrs, uint32_t count) {
  g_funcs.assign(addrs, addrs + count);
  uint32_t lo = 0xffffffffu, hi = 0;
  for (uint32_t a : g_funcs) lo = std::min(lo, a), hi = std::max(hi, a + 1);
  g_text_lo = lo;
  g_text_size = hi - lo;
  g_index = (uint16_t*)calloc(g_text_size, sizeof(uint16_t));
  for (uint32_t i = 0; i < count; i++) g_index[g_funcs[i] - lo] = (uint16_t)(i + 1);
}

// Runtime functions the module imports, as function-table references (stable regardless of how
// the toolchain names exports). Order matches RUNTIME_IMPORTS in recomp/src/emit-wasm.ts.
EMSCRIPTEN_KEEPALIVE uint32_t web_game_import(uint32_t i) {
  static const uintptr_t fns[] = {
      (uintptr_t)&rt_hle,      (uintptr_t)&rt_dispatch_ext, (uintptr_t)&rt_trap_id, (uintptr_t)&rt_cpuid,
      (uintptr_t)&rt_rep_movs, (uintptr_t)&rt_rep_stos,     (uintptr_t)&rt_rcl,     (uintptr_t)&rt_rcr,
      (uintptr_t)&rt_f80_load, (uintptr_t)&rt_f80_store,    (uintptr_t)&rt_fist16,  (uintptr_t)&rt_fist32,
      (uintptr_t)&rt_fist64,   (uintptr_t)&rt_frndint,      (uintptr_t)&rt_fxam,    (uintptr_t)&rt_sin,
      (uintptr_t)&rt_cos,      (uintptr_t)&rt_tan,          (uintptr_t)&rt_atan2,   (uintptr_t)&rt_log2,
      (uintptr_t)&rt_f2xm1,    (uintptr_t)&rt_fscale,       (uintptr_t)&rt_fprem,   (uintptr_t)&rt_fprem1,
      (uintptr_t)&rt_fnsave,   (uintptr_t)&rt_fnstenv,      (uintptr_t)&rt_frstor,  (uintptr_t)&rt_fldenv,
  };
  return i < sizeof fns / sizeof fns[0] ? (uint32_t)fns[i] : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t web_game_bytes() { return (uint32_t)(uintptr_t)g_game_bytes; }
EMSCRIPTEN_KEEPALIVE uint32_t web_game_len() { return g_game_len; }
EMSCRIPTEN_KEEPALIVE uint32_t web_game_index() { return (uint32_t)(uintptr_t)g_index; }

}  // extern "C"

// Instantiates the game module in the calling worker on first use, then calls its dispatcher.
EM_JS(void, game_call_js, (uint32_t c, uint32_t addr), {
  let g = globalThis.__shrek2Game;
  if (!g) {
    const ptr = Module._web_game_bytes(), len = Module._web_game_len();
    const module = new WebAssembly.Module(HEAPU8.slice(ptr, ptr + len));
    const names = ["rt_hle", "rt_dispatch", "rt_trap_id", "rt_cpuid", "rt_rep_movs", "rt_rep_stos", "rt_rcl", "rt_rcr",
                   "rt_f80_load", "rt_f80_store", "rt_fist16", "rt_fist32", "rt_fist64", "rt_frndint", "rt_fxam", "rt_sin",
                   "rt_cos", "rt_tan", "rt_atan2", "rt_log2", "rt_f2xm1", "rt_fscale", "rt_fprem", "rt_fprem1",
                   "rt_fnsave", "rt_fnstenv", "rt_frstor", "rt_fldenv"];
    const env = { memory: wasmMemory, index_base: new WebAssembly.Global({ value: "i32", mutable: false }, Module._web_game_index()) };
    names.forEach((n, i) => { env[n] = wasmTable.get(Module._web_game_import(i)); });
    const traps = WebAssembly.Module.customSections(module, "shrek2.traps");
    globalThis.__shrek2Traps = traps.length ? JSON.parse(new TextDecoder().decode(traps[0])) : [];
    g = globalThis.__shrek2Game = new WebAssembly.Instance(module, { env });
  }
  g.exports.dispatch(c, addr);
});

EM_JS(char*, game_trap_message, (uint32_t id), {
  const t = globalThis.__shrek2Traps;
  return stringToNewUTF8(t && id < t.length ? t[id] : "trap #" + id);
});

namespace rt {

bool game_owns(uint32_t target) {
  uint32_t off = target - g_text_lo;
  return off < g_text_size && g_index && g_index[off];
}

void game_call(Cpu* c, uint32_t target) { game_call_js((uint32_t)(uintptr_t)c, target); }

uint32_t game_function_count() { return (uint32_t)g_funcs.size(); }

}  // namespace rt

extern "C" void rt_trap_id(Cpu* c, uint32_t id) {
  char* msg = game_trap_message(id);
  rt::fatal("guest trap: %s", msg);
  (void)c;
}
