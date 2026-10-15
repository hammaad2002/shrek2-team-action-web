// user32.dll (+ gdi32 and winmm bits): window classes, one game window, and a message queue fed
// by the platform layer.
#include <chrono>
#include <deque>
#include <map>
#include <mutex>

#include "../core/objects.h"
#include "../platform/platform.h"

using namespace rt;

namespace {
const char* kDll = "user32.dll";

enum : uint32_t {
  WM_NULL = 0x0000, WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006,
  WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008, WM_CLOSE = 0x0010, WM_QUIT = 0x0012, WM_ACTIVATEAPP = 0x001c,
  WM_NCCREATE = 0x0081, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104,
  WM_SYSKEYUP = 0x0105, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
  WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020a,
};

struct WindowClass {
  uint32_t wndproc;
  std::string name;
};
struct Msg {
  uint32_t hwnd, message, wparam, lparam;
};

std::mutex g_mutex;
std::map<std::string, WindowClass> g_classes;
uint32_t g_hwnd = 0, g_wndproc = 0;
int g_width = 640, g_height = 480;
std::deque<Msg> g_queue;
int g_cursor_count = 0;
const auto g_start = std::chrono::steady_clock::now();

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

uint32_t now_ms() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
}

uint32_t send(Cpu* c, uint32_t msg, uint32_t wparam, uint32_t lparam) {
  if (!g_wndproc) return 0;
  return guest_call(c, g_wndproc, {g_hwnd, msg, wparam, lparam});
}

void post(uint32_t msg, uint32_t wparam, uint32_t lparam) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queue.push_back({g_hwnd, msg, wparam, lparam});
}

// Translate platform input into window messages.
void pump() {
  std::vector<plat::InputEvent> evs;
  plat::poll_events(evs);
  for (auto& e : evs) {
    uint32_t keyl = 1u | (e.scancode & 0xff) << 16 | ((e.scancode & 0x100) ? 1u << 24 : 0u);
    switch (e.kind) {
      case plat::InputEvent::KeyDown:
        post(e.vk == 0x12 ? WM_SYSKEYDOWN : WM_KEYDOWN, e.vk, keyl | (e.repeat ? 1u << 30 : 0u));
        break;
      case plat::InputEvent::KeyUp:
        post(e.vk == 0x12 ? WM_SYSKEYUP : WM_KEYUP, e.vk, keyl | 3u << 30);
        break;
      case plat::InputEvent::Char:
        post(WM_CHAR, e.ch, 1);
        break;
      case plat::InputEvent::MouseMove:
        post(WM_MOUSEMOVE, 0, (uint32_t)(e.y & 0xffff) << 16 | (uint32_t)(e.x & 0xffff));
        break;
      case plat::InputEvent::MouseDown:
      case plat::InputEvent::MouseUp: {
        static const uint32_t down[] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
        static const uint32_t up[] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
        if (e.button < 3)
          post(e.kind == plat::InputEvent::MouseDown ? down[e.button] : up[e.button], 0,
               (uint32_t)(e.y & 0xffff) << 16 | (uint32_t)(e.x & 0xffff));
        break;
      }
      case plat::InputEvent::MouseWheel:
        post(WM_MOUSEWHEEL, (uint32_t)(e.dy * 120) << 16, 0);
        break;
      case plat::InputEvent::FocusGained:
        post(WM_ACTIVATEAPP, 1, 0);
        post(WM_ACTIVATE, 1, 0);
        break;
      case plat::InputEvent::FocusLost:
        post(WM_ACTIVATE, 0, 0);
        post(WM_ACTIVATEAPP, 0, 0);
        break;
      case plat::InputEvent::Quit:
        post(WM_CLOSE, 0, 0);
        break;
    }
  }
}
}  // namespace

HLE_STDCALL(kDll, RegisterClassExA, 1) {
  uint32_t wc = arg(c, 0);
  std::string name = read_cstr(ld32(wc + 40));
  std::lock_guard<std::mutex> lock(g_mutex);
  g_classes[lower(name)] = {ld32(wc + 8), name};
  log(Log::Info, "RegisterClassExA(\"%s\", wndproc=%08x)", name.c_str(), ld32(wc + 8));
  return 0xc000 + (uint32_t)g_classes.size();
}

HLE_STDCALL(kDll, CreateWindowExA, 12) {
  std::string cls = lower(read_cstr(arg(c, 1)));
  std::string title = arg(c, 2) ? read_cstr(arg(c, 2)) : "";
  int w = (int)arg(c, 6), h = (int)arg(c, 7);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) {
      set_last_error(1407);  // ERROR_CANNOT_FIND_WND_CLASS
      return 0;
    }
    if (g_hwnd) fatal("the game created a second window");
    g_hwnd = 0x00010010;
    g_wndproc = it->second.wndproc;
    g_width = w > 0 && w < 10000 ? w : 640;
    g_height = h > 0 && h < 10000 ? h : 480;
  }
  log(Log::Info, "CreateWindowExA(\"%s\", \"%s\", %dx%d)", cls.c_str(), title.c_str(), g_width, g_height);
  plat::create_window(title, g_width, g_height);
  // CREATESTRUCTA for WM_NCCREATE/WM_CREATE.
  uint32_t cs = sys_alloc(48);
  st32(cs + 0, arg(c, 11));  // lpCreateParams
  st32(cs + 4, arg(c, 10));  // hInstance
  st32(cs + 8, arg(c, 9));   // hMenu
  st32(cs + 12, arg(c, 8));  // hwndParent
  st32(cs + 16, (uint32_t)g_height);
  st32(cs + 20, (uint32_t)g_width);
  st32(cs + 24, arg(c, 5));
  st32(cs + 28, arg(c, 4));
  st32(cs + 32, arg(c, 3));  // style
  st32(cs + 36, arg(c, 2));  // name
  st32(cs + 40, arg(c, 1));  // class
  st32(cs + 44, arg(c, 0));  // exstyle
  if (!send(c, WM_NCCREATE, 0, cs)) log(Log::Warn, "WM_NCCREATE returned FALSE (continuing)");
  if ((int32_t)send(c, WM_CREATE, 0, cs) == -1) {
    g_hwnd = 0;
    return 0;
  }
  send(c, WM_SIZE, 0, (uint32_t)g_height << 16 | (uint32_t)g_width);
  send(c, WM_MOVE, 0, 0);
  return g_hwnd;
}

HLE_STDCALL(kDll, ShowWindow, 2) {
  static bool shown = false;
  if (!shown) {
    shown = true;
    send(c, WM_ACTIVATEAPP, 1, 0);
    send(c, WM_ACTIVATE, 1, 0);
    send(c, WM_SETFOCUS, 0, 0);
  }
  return 0;
}

HLE_STDCALL(kDll, PeekMessageA, 5) {
  uint32_t msg = arg(c, 0), remove = arg(c, 4) & 1;
  pump();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_queue.empty()) return 0;
  Msg m = g_queue.front();
  if (remove) g_queue.pop_front();
  st32(msg + 0, m.hwnd);
  st32(msg + 4, m.message);
  st32(msg + 8, m.wparam);
  st32(msg + 12, m.lparam);
  st32(msg + 16, now_ms());
  st32(msg + 20, 0);
  st32(msg + 24, 0);
  return 1;
}

HLE_STDCALL(kDll, TranslateMessage, 1) { return 0; }

HLE_STDCALL(kDll, DispatchMessageA, 1) {
  uint32_t m = arg(c, 0);
  if (ld32(m + 4) == WM_QUIT) return 0;
  return send(c, ld32(m + 4), ld32(m + 8), ld32(m + 12));
}

HLE_STDCALL(kDll, DefWindowProcA, 4) {
  uint32_t msg = arg(c, 1);
  switch (msg) {
    case WM_NCCREATE:
      return 1;
    case WM_CLOSE:
      send(c, WM_DESTROY, 0, 0);
      return 0;
    default:
      return 0;
  }
}

HLE_STDCALL(kDll, PostQuitMessage, 1) {
  post(WM_QUIT, arg(c, 0), 0);
  return 0;
}

HLE_STDCALL(kDll, ShowCursor, 1) {
  g_cursor_count += arg(c, 0) ? 1 : -1;
  plat::show_cursor(g_cursor_count >= 0);
  return (uint32_t)g_cursor_count;
}

HLE_STDCALL(kDll, GetSystemMetrics, 1) {
  int w = 0, h = 0;
  plat::get_display_size(&w, &h);
  switch (arg(c, 0)) {
    case 0: return (uint32_t)w;  // SM_CXSCREEN
    case 1: return (uint32_t)h;  // SM_CYSCREEN
    default: return 0;
  }
}

HLE_STDCALL(kDll, AdjustWindowRect, 3) { return 1; }

HLE_STDCALL(kDll, SystemParametersInfoA, 4) {
  uint32_t action = arg(c, 0), ui = arg(c, 1), pv = arg(c, 2);
  // Accessibility key queries (sticky/toggle/filter keys) get "feature off" structures.
  if ((action == 0x3a || action == 0x34 || action == 0x32) && pv) {
    uint32_t size = ld32(pv) ? ld32(pv) : ui;
    if (size > 4 && size < 64) {
      memset(host(pv + 4), 0, size - 4);
    }
  }
  return 1;
}

HLE_STDCALL(kDll, FindWindowExA, 4) { return 0; }
HLE_STDCALL(kDll, BringWindowToTop, 1) { return 1; }
HLE_STDCALL(kDll, SetForegroundWindow, 1) { return 1; }
HLE_STDCALL(kDll, InvalidateRect, 3) { return 1; }
HLE_STDCALL(kDll, ValidateRect, 2) { return 1; }
HLE_STDCALL(kDll, LoadIconA, 2) { return 0x00020020; }
HLE_STDCALL(kDll, GetActiveWindow, 0) { return g_hwnd; }

HLE_STDCALL(kDll, MapVirtualKeyA, 2) {
  // MAPVK_VK_TO_VSC (0) and MAPVK_VSC_TO_VK (1) for the keys games use.
  static const uint8_t vk_to_sc[256] = {
      0,    0,    0,    0x46, 0,    0,    0,    0,    0x0e, 0x0f, 0,    0,    0x4c, 0x1c, 0,    0,     // 00
      0x2a, 0x1d, 0x38, 0,    0x3a, 0,    0,    0,    0,    0,    0,    0x01, 0,    0,    0,    0,     // 10
      0x39, 0x49, 0x51, 0x4f, 0x47, 0x4b, 0x48, 0x4d, 0x50, 0,    0,    0,    0x54, 0x52, 0x53, 0,     // 20
      0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0,    0,    0,    0,    0,    0,     // 30
      0,    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,  // 40
      0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x5b, 0x5c, 0x5d, 0,    0,     // 50
      0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x37, 0x4e, 0,    0x4a, 0x53, 0x35,  // 60
      0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0,    0,    0,    0,     // 70
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // 80
      0x45, 0x46, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // 90
      0x2a, 0x36, 0x1d, 0x1d, 0x38, 0x38, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // a0
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x27, 0x0d, 0x33, 0x0c, 0x34, 0x35,  // b0
      0x29, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // c0
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x1a, 0x2b, 0x1b, 0x28, 0,     // d0
      0,    0,    0x56, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // e0
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0};    // f0
  uint32_t code = arg(c, 0), type = arg(c, 1);
  if (type == 0) return vk_to_sc[code & 0xff];
  if (type == 1 || type == 3) {
    for (int vk = 0; vk < 256; vk++)
      if (vk_to_sc[vk] == (code & 0xff) && vk >= 0x08) return (uint32_t)vk;
    return 0;
  }
  return 0;
}

// gdi32 and winmm.
HLE_STDCALL("gdi32.dll", DeleteObject, 1) { return 1; }
HLE_STDCALL("winmm.dll", timeGetTime, 0) { return now_ms(); }
