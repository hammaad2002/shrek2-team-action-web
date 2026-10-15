// Native platform layer on SDL2: window, keyboard/mouse input (as Windows VK codes and
// DirectInput/PC set-1 scan codes).
#include <SDL2/SDL.h>

#include <cstring>
#include <mutex>

#include "../core/rt.h"
#include "platform.h"

namespace plat {
namespace {

SDL_Window* g_window = nullptr;
uint8_t g_keys[256];
MouseState g_mouse{};
std::mutex g_mouse_mutex;

struct KeyMap {
  SDL_Scancode sdl;
  uint8_t vk;
  uint16_t dik;  // set-1 scan code; 0x100 marks extended (E0-prefixed) keys
};

const KeyMap kKeys[] = {
    {SDL_SCANCODE_ESCAPE, 0x1b, 0x01}, {SDL_SCANCODE_1, '1', 0x02}, {SDL_SCANCODE_2, '2', 0x03},
    {SDL_SCANCODE_3, '3', 0x04}, {SDL_SCANCODE_4, '4', 0x05}, {SDL_SCANCODE_5, '5', 0x06}, {SDL_SCANCODE_6, '6', 0x07},
    {SDL_SCANCODE_7, '7', 0x08}, {SDL_SCANCODE_8, '8', 0x09}, {SDL_SCANCODE_9, '9', 0x0a}, {SDL_SCANCODE_0, '0', 0x0b},
    {SDL_SCANCODE_MINUS, 0xbd, 0x0c}, {SDL_SCANCODE_EQUALS, 0xbb, 0x0d}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0e},
    {SDL_SCANCODE_TAB, 0x09, 0x0f}, {SDL_SCANCODE_Q, 'Q', 0x10}, {SDL_SCANCODE_W, 'W', 0x11}, {SDL_SCANCODE_E, 'E', 0x12},
    {SDL_SCANCODE_R, 'R', 0x13}, {SDL_SCANCODE_T, 'T', 0x14}, {SDL_SCANCODE_Y, 'Y', 0x15}, {SDL_SCANCODE_U, 'U', 0x16},
    {SDL_SCANCODE_I, 'I', 0x17}, {SDL_SCANCODE_O, 'O', 0x18}, {SDL_SCANCODE_P, 'P', 0x19},
    {SDL_SCANCODE_LEFTBRACKET, 0xdb, 0x1a}, {SDL_SCANCODE_RIGHTBRACKET, 0xdd, 0x1b}, {SDL_SCANCODE_RETURN, 0x0d, 0x1c},
    {SDL_SCANCODE_LCTRL, 0x11, 0x1d}, {SDL_SCANCODE_A, 'A', 0x1e}, {SDL_SCANCODE_S, 'S', 0x1f}, {SDL_SCANCODE_D, 'D', 0x20},
    {SDL_SCANCODE_F, 'F', 0x21}, {SDL_SCANCODE_G, 'G', 0x22}, {SDL_SCANCODE_H, 'H', 0x23}, {SDL_SCANCODE_J, 'J', 0x24},
    {SDL_SCANCODE_K, 'K', 0x25}, {SDL_SCANCODE_L, 'L', 0x26}, {SDL_SCANCODE_SEMICOLON, 0xba, 0x27},
    {SDL_SCANCODE_APOSTROPHE, 0xde, 0x28}, {SDL_SCANCODE_GRAVE, 0xc0, 0x29}, {SDL_SCANCODE_LSHIFT, 0x10, 0x2a},
    {SDL_SCANCODE_BACKSLASH, 0xdc, 0x2b}, {SDL_SCANCODE_Z, 'Z', 0x2c}, {SDL_SCANCODE_X, 'X', 0x2d},
    {SDL_SCANCODE_C, 'C', 0x2e}, {SDL_SCANCODE_V, 'V', 0x2f}, {SDL_SCANCODE_B, 'B', 0x30}, {SDL_SCANCODE_N, 'N', 0x31},
    {SDL_SCANCODE_M, 'M', 0x32}, {SDL_SCANCODE_COMMA, 0xbc, 0x33}, {SDL_SCANCODE_PERIOD, 0xbe, 0x34},
    {SDL_SCANCODE_SLASH, 0xbf, 0x35}, {SDL_SCANCODE_RSHIFT, 0x10, 0x36}, {SDL_SCANCODE_KP_MULTIPLY, 0x6a, 0x37},
    {SDL_SCANCODE_LALT, 0x12, 0x38}, {SDL_SCANCODE_SPACE, 0x20, 0x39}, {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3a},
    {SDL_SCANCODE_F1, 0x70, 0x3b}, {SDL_SCANCODE_F2, 0x71, 0x3c}, {SDL_SCANCODE_F3, 0x72, 0x3d},
    {SDL_SCANCODE_F4, 0x73, 0x3e}, {SDL_SCANCODE_F5, 0x74, 0x3f}, {SDL_SCANCODE_F6, 0x75, 0x40},
    {SDL_SCANCODE_F7, 0x76, 0x41}, {SDL_SCANCODE_F8, 0x77, 0x42}, {SDL_SCANCODE_F9, 0x78, 0x43},
    {SDL_SCANCODE_F10, 0x79, 0x44}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45}, {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46},
    {SDL_SCANCODE_KP_7, 0x67, 0x47}, {SDL_SCANCODE_KP_8, 0x68, 0x48}, {SDL_SCANCODE_KP_9, 0x69, 0x49},
    {SDL_SCANCODE_KP_MINUS, 0x6d, 0x4a}, {SDL_SCANCODE_KP_4, 0x64, 0x4b}, {SDL_SCANCODE_KP_5, 0x65, 0x4c},
    {SDL_SCANCODE_KP_6, 0x66, 0x4d}, {SDL_SCANCODE_KP_PLUS, 0x6b, 0x4e}, {SDL_SCANCODE_KP_1, 0x61, 0x4f},
    {SDL_SCANCODE_KP_2, 0x62, 0x50}, {SDL_SCANCODE_KP_3, 0x63, 0x51}, {SDL_SCANCODE_KP_0, 0x60, 0x52},
    {SDL_SCANCODE_KP_PERIOD, 0x6e, 0x53}, {SDL_SCANCODE_F11, 0x7a, 0x57}, {SDL_SCANCODE_F12, 0x7b, 0x58},
    {SDL_SCANCODE_KP_ENTER, 0x0d, 0x11c}, {SDL_SCANCODE_RCTRL, 0x11, 0x11d}, {SDL_SCANCODE_KP_DIVIDE, 0x6f, 0x135},
    {SDL_SCANCODE_RALT, 0x12, 0x138}, {SDL_SCANCODE_HOME, 0x24, 0x147}, {SDL_SCANCODE_UP, 0x26, 0x148},
    {SDL_SCANCODE_PAGEUP, 0x21, 0x149}, {SDL_SCANCODE_LEFT, 0x25, 0x14b}, {SDL_SCANCODE_RIGHT, 0x27, 0x14d},
    {SDL_SCANCODE_END, 0x23, 0x14f}, {SDL_SCANCODE_DOWN, 0x28, 0x150}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x151},
    {SDL_SCANCODE_INSERT, 0x2d, 0x152}, {SDL_SCANCODE_DELETE, 0x2e, 0x153},
};

const KeyMap* lookup(SDL_Scancode s) {
  for (auto& k : kKeys)
    if (k.sdl == s) return &k;
  return nullptr;
}

// DirectInput keyboard indices: extended keys use 0x80 | scancode (e.g. DIK_UP = 0xC8).
uint8_t dik_index(uint16_t dik) { return (uint8_t)((dik & 0x100) ? 0x80 | (dik & 0x7f) : dik); }

}  // namespace

bool init() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
    rt::log(rt::Log::Error, "SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  return true;
}

void create_window(const std::string& title, int width, int height) {
  if (g_window) return;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  g_window = SDL_CreateWindow(("Shrek 2 Team Action - " + title).c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!g_window) rt::fatal("SDL_CreateWindow failed: %s", SDL_GetError());
}

bool has_window() { return g_window != nullptr; }

void set_window_size(int width, int height) {
  if (g_window) SDL_SetWindowSize(g_window, width, height);
}

void get_display_size(int* width, int* height) {
  SDL_DisplayMode m;
  if (SDL_GetDesktopDisplayMode(0, &m) == 0) {
    *width = m.w;
    *height = m.h;
  } else {
    *width = 1920;
    *height = 1080;
  }
}

void get_window_size(int* width, int* height) {
  if (g_window) SDL_GL_GetDrawableSize(g_window, width, height);
  else *width = 640, *height = 480;
}

void show_cursor(bool visible) { SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE); }

void poll_events(std::vector<InputEvent>& out) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    InputEvent ev{};
    switch (e.type) {
      case SDL_QUIT:
        ev.kind = InputEvent::Quit;
        out.push_back(ev);
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        const KeyMap* k = lookup(e.key.keysym.scancode);
        if (!k) break;
        bool down = e.type == SDL_KEYDOWN;
        g_keys[dik_index(k->dik)] = down ? 0x80 : 0;
        ev.kind = down ? InputEvent::KeyDown : InputEvent::KeyUp;
        ev.vk = k->vk;
        ev.scancode = k->dik;
        ev.repeat = e.key.repeat != 0;
        out.push_back(ev);
        break;
      }
      case SDL_TEXTINPUT:
        ev.kind = InputEvent::Char;
        ev.ch = (uint8_t)e.text.text[0];
        out.push_back(ev);
        break;
      case SDL_MOUSEMOTION: {
        std::lock_guard<std::mutex> lock(g_mouse_mutex);
        g_mouse.dx += e.motion.xrel;
        g_mouse.dy += e.motion.yrel;
        ev.kind = InputEvent::MouseMove;
        ev.x = e.motion.x;
        ev.y = e.motion.y;
        out.push_back(ev);
        break;
      }
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        uint32_t b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
        bool down = e.type == SDL_MOUSEBUTTONDOWN;
        {
          std::lock_guard<std::mutex> lock(g_mouse_mutex);
          g_mouse.buttons[b] = down ? 0x80 : 0;
        }
        ev.kind = down ? InputEvent::MouseDown : InputEvent::MouseUp;
        ev.button = b;
        ev.x = e.button.x;
        ev.y = e.button.y;
        out.push_back(ev);
        break;
      }
      case SDL_MOUSEWHEEL: {
        std::lock_guard<std::mutex> lock(g_mouse_mutex);
        g_mouse.wheel += e.wheel.y * 120;
        ev.kind = InputEvent::MouseWheel;
        ev.dy = e.wheel.y;
        out.push_back(ev);
        break;
      }
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
          ev.kind = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? InputEvent::FocusGained : InputEvent::FocusLost;
          if (ev.kind == InputEvent::FocusLost) memset(g_keys, 0, sizeof g_keys);
          out.push_back(ev);
        }
        break;
      default:
        break;
    }
  }
}

const uint8_t* keyboard_state() { return g_keys; }

MouseState take_mouse_state() {
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  MouseState m = g_mouse;
  g_mouse.dx = g_mouse.dy = g_mouse.wheel = 0;
  return m;
}

}  // namespace plat
