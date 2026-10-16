#include "recorder.h"

#include <atomic>
#include <mutex>
#include <thread>

#include "../gles/executor.h"
#if defined(__EMSCRIPTEN__)
#include "../platform/web.h"
#endif

using namespace rt;

namespace d3d {
namespace {
cmd::Stream g_stream;
uint32_t g_next_id = 1;
}  // namespace

std::recursive_mutex& api_lock() {
  static std::recursive_mutex m;
  return m;
}

cmd::Stream& rec() { return g_stream; }

namespace {
Device* g_device = nullptr;
std::atomic<bool> g_app_active{true};
}  // namespace

void set_app_active(bool active) {
  std::lock_guard<std::recursive_mutex> lock(api_lock());
  g_app_active = active;
  if (!active && g_device && g_device->pp[8] == 0) {  // pp[8] = Windowed
    if (!g_device->lost) rt::log(rt::Log::Info, "D3D: application deactivated, fullscreen device lost");
    g_device->lost = true;
  }
}

bool app_active() { return g_app_active; }
uint32_t new_resource_id() { return g_next_id++; }

void recorder_create_device(Device* dev) {
  g_device = dev;
  dev->lost = false;
  // Reset() unbinds everything and returns to the default state (first creation: no-op).
  for (auto& t : dev->st.textures) {
    release_ref(t);
    t = nullptr;
  }
  for (auto& s : dev->st.streams) {
    release_ref(s.vb);
    s = {};
  }
  release_ref(dev->st.indices);
  dev->st.indices = nullptr;
  dev->st.vs = dev->st.ps = nullptr;
  dev->st.decl = nullptr;
  dev->st.fvf = 0;
  g_stream.op(cmd::OpResetState, {});
  init_default_state(dev->st);
  dev->st.rs[7] = dev->pp[9] ? 1 : 0;  // ZENABLE follows EnableAutoDepthStencil
  dev->st.viewport = Viewport{0, 0, dev->pp[0], dev->pp[1], 0.0f, 1.0f};
  dev->st.scissor = {0, 0, (int32_t)dev->pp[0], (int32_t)dev->pp[1]};
  // Drop the bindings of a previous configuration (Reset).
  for (auto& rt : dev->render_targets) {
    release_ref(rt);
    rt = nullptr;
  }
  release_ref(dev->depth_stencil);
  dev->depth_stencil = nullptr;
  release_ref(dev->auto_depth);  // the device's own reference
  dev->auto_depth = nullptr;
  if (!dev->back_buffer) {
    auto* bb = new Surface();
    bb->device = dev;
    bb->usage = USAGE_RENDERTARGET;
    bb->pool = POOL_DEFAULT;
    dev->back_buffer = bb;
    rt::com_wrap(bb, iface_surface());
  }
  dev->back_buffer->width = dev->pp[0];
  dev->back_buffer->height = dev->pp[1];
  dev->back_buffer->format = dev->pp[2];
  dev->back_buffer->pitch = row_pitch(dev->pp[2], dev->pp[0]);
  if (dev->pp[9]) {
    dev->auto_depth = create_surface(dev, dev->pp[0], dev->pp[1], dev->pp[10], USAGE_DEPTHSTENCIL, POOL_DEFAULT);
    rt::com_wrap(dev->auto_depth, iface_surface());
  }
  // The device holds a reference on its bound render target and depth buffer.
  dev->render_targets = {dev->back_buffer, nullptr, nullptr, nullptr};
  add_ref(dev->back_buffer);
  dev->depth_stencil = dev->auto_depth;
  add_ref(dev->auto_depth);
  // The executor starts from the same default state (init_default_state) and back buffer.
  g_stream.op(cmd::OpBackBuffer, {dev->pp[0], dev->pp[1], dev->pp[13] == 0x80000000u ? 0u : 1u, dev->pp[2]});
  g_stream.op(cmd::OpSetRenderTarget, {0, 1, 0, 0, 0});
  SurfaceRef d = surface_ref(dev->auto_depth);
  g_stream.op(cmd::OpSetDepthStencil, {d.kind, d.id});
  g_stream.op(cmd::OpSetRenderState, {7, dev->st.rs[7]});
}

#if defined(__EMSCRIPTEN__)

// The page's main thread owns WebGL: batches are queued and executed there every animation frame.
void flush(bool present) {
  size_t cap = g_stream.words.size();
  web::submit(std::move(g_stream.words), present);
  g_stream.words = {};
  g_stream.words.reserve(cap);
}

void flush_and_wait() {
  web::submit_and_wait(std::move(g_stream.words));
  g_stream.words = {};
}

#else

// Natively the GL context belongs to the first thread that flushes (the game's render thread);
// commands recorded on other threads simply wait in the stream until that thread flushes.
static std::thread::id g_gl_thread;
static bool g_gl_thread_set = false;

static bool on_gl_thread() {
  if (!g_gl_thread_set) {
    g_gl_thread = std::this_thread::get_id();
    g_gl_thread_set = true;
  }
  return g_gl_thread == std::this_thread::get_id();
}

void flush(bool) {
  if (!on_gl_thread()) return;
  gles::execute(g_stream.words.data(), g_stream.words.size());
  g_stream.clear();
}

void flush_and_wait() {
  if (!on_gl_thread()) rt::fatal("Direct3D readback requested from a thread without the GL context");
  flush();
}

#endif

}  // namespace d3d
