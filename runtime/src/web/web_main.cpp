// Web entry point. With PROXY_TO_PTHREAD, main() runs on a pthread and becomes the game's main
// thread; the page's main thread calls web_render_init() once and web_render_tick() every
// animation frame to execute the recorded Direct3D command stream with WebGL 2.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/wasmfs.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>

#include "../core/objects.h"
#include "../core/rt.h"
#include "../gles/executor.h"
#include "../hle/registry.h"
#include "../platform/platform.h"
#include "../platform/web.h"

namespace web {
namespace {

struct Batch {
  std::vector<uint32_t> words;
  bool present;
  uint64_t seq;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Batch> g_queue;
uint64_t g_submitted = 0, g_executed = 0, g_frames_queued = 0, g_frames_done = 0;
std::atomic<bool> g_gl{false};

uint64_t enqueue(std::vector<uint32_t>&& words, bool present) {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint64_t seq = ++g_submitted;
  g_queue.push_back({std::move(words), present, seq});
  if (present) g_frames_queued++;
  return seq;
}

}  // namespace

bool gl_ready() { return g_gl; }

void submit(std::vector<uint32_t>&& words, bool present) {
  enqueue(std::move(words), present);
  if (!present) return;
  // At most one frame waiting for the display.
  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait(lock, [] { return g_frames_queued - g_frames_done <= 1; });
}

void submit_and_wait(std::vector<uint32_t>&& words) {
  uint64_t seq = enqueue(std::move(words), false);
  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait(lock, [&] { return g_executed >= seq; });
}

}  // namespace web

extern "C" {

// Called once on the page's main thread after the canvas exists.
EMSCRIPTEN_KEEPALIVE int web_render_init(const char* canvas_selector) {
  EmscriptenWebGLContextAttributes a;
  emscripten_webgl_init_context_attributes(&a);
  a.majorVersion = 2;
  a.minorVersion = 0;
  a.alpha = false;
  a.depth = false;
  a.stencil = false;
  a.antialias = false;
  a.premultipliedAlpha = false;
  a.preserveDrawingBuffer = false;
  a.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
  a.enableExtensionsByDefault = true;
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context(canvas_selector, &a);
  if (ctx <= 0) {
    rt::log(rt::Log::Error, "WebGL 2 is not available (%d)", (int)ctx);
    return 0;
  }
  emscripten_webgl_make_context_current(ctx);
  emscripten_webgl_enable_extension(ctx, "WEBGL_compressed_texture_s3tc");
  emscripten_webgl_enable_extension(ctx, "EXT_texture_filter_anisotropic");
  web::g_gl = true;
  return 1;
}

// Called every animation frame on the page's main thread: executes queued command batches and
// stops after one presented frame so every frame reaches the screen.
EMSCRIPTEN_KEEPALIVE int web_render_tick() {
  using namespace web;
  int presented = 0;
  for (;;) {
    Batch b;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_queue.empty()) break;
      b = std::move(g_queue.front());
      g_queue.pop_front();
    }
    gles::execute(b.words.data(), b.words.size());
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_executed = b.seq;
      if (b.present) g_frames_done++;
    }
    g_cv.notify_all();
    if (b.present) {
      presented = 1;
      break;
    }
  }
  return presented;
}

}  // extern "C"

// argv: [install-dir] [registry-file]; the page mounts OPFS before starting the module.
int main(int argc, char** argv) {
  std::string install = argc > 1 ? argv[1] : "/opfs/shrek2";
  std::string reg = argc > 2 ? argv[2] : install + "/registry.txt";
  if (install.rfind("/opfs", 0) == 0) {
    backend_t opfs = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/opfs", 0777, opfs) != 0) rt::fatal("cannot mount OPFS at /opfs");
  }
  rt::memory_init();
  rt::vfs_init(install);
  rt::registry_init(reg);
  plat::init();
  std::ifstream f(install + "/Game/Shrek 2.exe", std::ios::binary);
  if (!f) rt::fatal("cannot open %s/Game/Shrek 2.exe (is the game installed?)", install.c_str());
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  rt::load_image(exe);
  rt::dispatch_init();
  rt::watchdog_start();
  rt::GuestThread* t = rt::create_guest_thread(0x100000);
  rt::bind_host_thread(t);
  rt::log(rt::Log::Info, "starting guest at entry %08x", rt::image_entry());
  uint32_t code = rt::guest_call(&t->cpu, rt::image_entry(), {});
  rt::process_exit(code);
}
