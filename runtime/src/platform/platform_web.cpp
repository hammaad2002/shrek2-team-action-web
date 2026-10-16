// Web platform layer (Emscripten). The game runs on pthreads (Web Workers); the page's main
// thread owns the canvas and its WebGL 2 context, runs the render-command executor once per
// animation frame, and forwards DOM input through the exported web_* functions. Audio is rendered
// by a feeder thread into a shared ring buffer that an AudioWorklet plays.
#include <emscripten.h>
#include <emscripten/html5.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "../core/rt.h"
#include "platform.h"
#include "web.h"

namespace plat {
namespace {

std::mutex g_input_mutex;
std::deque<InputEvent> g_input;
uint8_t g_keys[256];
MouseState g_mouse{};
std::atomic<int> g_canvas_w{640}, g_canvas_h{480};
std::atomic<bool> g_window{false};

uint8_t dik_index(uint32_t dik) { return (uint8_t)((dik & 0x100) ? 0x80 | (dik & 0x7f) : dik); }

void push(const InputEvent& e) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  g_input.push_back(e);
}

}  // namespace

bool init() { return true; }

void create_window(const std::string& title, int width, int height) {
  g_window = true;
  (void)title, (void)width, (void)height;  // the page decides the canvas size
}

bool has_window() { return g_window; }
void set_window_size(int, int) {}

void get_display_size(int* width, int* height) {
  *width = g_canvas_w;
  *height = g_canvas_h;
}

void get_window_size(int* width, int* height) {
  *width = g_canvas_w;
  *height = g_canvas_h;
}

// The game hides the cursor when it goes fullscreen; mirror that on the canvas (page thread).
void show_cursor(bool visible) {
  MAIN_THREAD_ASYNC_EM_ASM({ if (Module["canvas"]) Module["canvas"].style.cursor = $0 ? "" : "none"; }, visible ? 1 : 0);
}

void poll_events(std::vector<InputEvent>& out) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  while (!g_input.empty()) {
    out.push_back(g_input.front());
    g_input.pop_front();
  }
}

const uint8_t* keyboard_state() { return g_keys; }

MouseState take_mouse_state() {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  MouseState m = g_mouse;
  g_mouse.dx = g_mouse.dy = g_mouse.wheel = 0;
  return m;
}

void frame_presented(uint64_t) {}

// The context is created by web_render_init() on the page's main thread, where the executor runs.
bool gl_create_context() { return web::gl_ready(); }
void gl_swap() {}  // the browser presents when the animation-frame callback returns
void gl_set_swap_interval(int) {}

// ---- Audio ------------------------------------------------------------------------------------

namespace {
struct AudioRing {
  std::atomic<uint32_t> write{0}, read{0};  // frame counters (wrap freely)
  uint32_t frames = 0;                       // capacity (power of two)
  uint32_t rate = 44100;
  float* data = nullptr;                     // interleaved stereo
};
AudioRing g_ring;
std::atomic<bool> g_audio_running{false};
}  // namespace

bool audio_open(int rate, void (*render)(float* out, int frames)) {
  if (!g_ring.data) {
    rt::log(rt::Log::Warn, "audio: no output ring (the page did not create an AudioContext)");
    return false;
  }
  if (g_audio_running.exchange(true)) return true;
  (void)rate;  // the page creates its AudioContext at the mixer rate
  std::thread([render] {
    // Keep ~90 ms buffered ahead of the worklet.
    const uint32_t target = g_ring.rate * 90 / 1000, chunk = 256;
    std::vector<float> buf(chunk * 2);
    for (;;) {
      uint32_t w = g_ring.write.load(std::memory_order_relaxed), r = g_ring.read.load(std::memory_order_acquire);
      if (w - r + chunk > target) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }
      render(buf.data(), (int)chunk);
      for (uint32_t i = 0; i < chunk; i++) {
        uint32_t slot = (w + i) & (g_ring.frames - 1);
        g_ring.data[2 * slot] = buf[2 * i];
        g_ring.data[2 * slot + 1] = buf[2 * i + 1];
      }
      g_ring.write.store(w + chunk, std::memory_order_release);
    }
  }).detach();
  rt::log(rt::Log::Info, "audio: feeding a %u-frame ring at %u Hz", g_ring.frames, g_ring.rate);
  return true;
}

}  // namespace plat

// ---- Exports for the page ------------------------------------------------------------------------

extern "C" {

// Allocates the audio ring; returns its address (layout: write u32, read u32, frames u32, rate u32,
// data pointer u32) so the AudioWorklet can read it from the shared memory.
EMSCRIPTEN_KEEPALIVE uint32_t web_audio_ring_create(uint32_t rate) {
  using namespace plat;
  static uint32_t header[5];
  g_ring.frames = 8192;
  g_ring.rate = rate;
  g_ring.data = new float[g_ring.frames * 2]();
  // The worklet reads the live atomics through these addresses.
  header[0] = (uint32_t)(uintptr_t)&g_ring.write;
  header[1] = (uint32_t)(uintptr_t)&g_ring.read;
  header[2] = g_ring.frames;
  header[3] = g_ring.rate;
  header[4] = (uint32_t)(uintptr_t)g_ring.data;
  return (uint32_t)(uintptr_t)header;
}

EMSCRIPTEN_KEEPALIVE void web_canvas_size(int w, int h) {
  plat::g_canvas_w = w;
  plat::g_canvas_h = h;
}

// kind: 0 key down, 1 key up, 2 character.
EMSCRIPTEN_KEEPALIVE void web_key(int kind, uint32_t vk, uint32_t dik, uint32_t ch) {
  using namespace plat;
  InputEvent e{};
  if (kind == 2) {
    e.kind = InputEvent::Char;
    e.ch = ch;
    push(e);
    return;
  }
  bool down = kind == 0;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (dik) g_keys[dik_index(dik)] = down ? 0x80 : 0;
  }
  e.kind = down ? InputEvent::KeyDown : InputEvent::KeyUp;
  e.vk = vk;
  e.scancode = dik;
  push(e);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_move(int x, int y, int dx, int dy) {
  using namespace plat;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_mouse.dx += dx;
    g_mouse.dy += dy;
  }
  InputEvent e{};
  e.kind = InputEvent::MouseMove;
  e.x = x, e.y = y;
  push(e);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_button(int button, int down, int x, int y) {
  using namespace plat;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (button >= 0 && button < 4) g_mouse.buttons[button] = down ? 0x80 : 0;
  }
  InputEvent e{};
  e.kind = down ? InputEvent::MouseDown : InputEvent::MouseUp;
  e.button = (uint32_t)button;
  e.x = x, e.y = y;
  push(e);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_wheel(int dy) {
  using namespace plat;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_mouse.wheel += dy * 120;
  }
  InputEvent e{};
  e.kind = InputEvent::MouseWheel;
  e.dy = dy;
  push(e);
}

EMSCRIPTEN_KEEPALIVE void web_focus(int focused) {
  using namespace plat;
  if (!focused) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    memset(g_keys, 0, sizeof g_keys);
  }
  InputEvent e{};
  e.kind = focused ? InputEvent::FocusGained : InputEvent::FocusLost;
  push(e);
}

}  // extern "C"
