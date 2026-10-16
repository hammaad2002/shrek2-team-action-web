// Native platform layer on SDL2: window, keyboard/mouse input (as Windows VK codes and
// DirectInput/PC set-1 scan codes) and gamepads (SDL GameController, or scripted for tests).
#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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
  // Let SIGINT/SIGTERM terminate the process instead of becoming a quit event the game may ignore.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
    rt::log(rt::Log::Error, "SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  return true;
}

void create_window(const std::string& title, int width, int height) {
  if (g_window) return;
  // OpenGL ES 3.0: the same API and shading language the web build gets from WebGL 2.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  g_window = SDL_CreateWindow(("Shrek 2 Team Action - " + title).c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (getenv("SHREK2_HIDDEN") ? SDL_WINDOW_HIDDEN : 0));
  if (!g_window) rt::fatal("SDL_CreateWindow failed: %s", SDL_GetError());
}

bool has_window() { return g_window != nullptr; }

bool gl_create_context() {
  if (!g_window) create_window("Shrek 2", 640, 480);
  SDL_GLContext ctx = SDL_GL_CreateContext(g_window);
  if (!ctx) {
    rt::log(rt::Log::Error, "SDL_GL_CreateContext (OpenGL ES 3.0) failed: %s", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(g_window, ctx);
  return true;
}

void gl_swap() { SDL_GL_SwapWindow(g_window); }

void gl_set_swap_interval(int interval) { SDL_GL_SetSwapInterval(interval); }

namespace {
void (*g_audio_render)(float*, int) = nullptr;
void audio_callback(void*, Uint8* stream, int len) { g_audio_render((float*)stream, len / (int)(2 * sizeof(float))); }
}  // namespace

bool audio_open(int rate, void (*render)(float* out, int frames)) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    rt::log(rt::Log::Warn, "SDL audio init failed: %s", SDL_GetError());
    return false;
  }
  g_audio_render = render;
  SDL_AudioSpec want{}, have{};
  want.freq = rate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_callback;
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!dev) {
    rt::log(rt::Log::Warn, "SDL_OpenAudioDevice failed: %s", SDL_GetError());
    return false;
  }
  SDL_PauseAudioDevice(dev, 0);
  rt::log(rt::Log::Info, "audio: %d Hz, %d-frame buffer", have.freq, have.samples);
  return true;
}

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

namespace {
void scripted_events(std::vector<InputEvent>& out);
void scripted_pads();

// Controllers fill player slots in the order they are connected.
SDL_GameController* g_controllers[kMaxGamepads] = {};

void controller_added(int device_index) {
  for (auto& c : g_controllers)
    if (c && SDL_JoystickGetDeviceInstanceID(device_index) == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c))) return;
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    if (g_controllers[slot]) continue;
    g_controllers[slot] = SDL_GameControllerOpen(device_index);
    if (g_controllers[slot]) rt::log(rt::Log::Info, "gamepad: %s is player %d", SDL_GameControllerName(g_controllers[slot]), slot + 1);
    return;
  }
}

void controller_removed(SDL_JoystickID id) {
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    SDL_GameController* c = g_controllers[slot];
    if (c && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) {
      rt::log(rt::Log::Info, "gamepad: player %d's controller disconnected", slot + 1);
      SDL_GameControllerClose(c);
      g_controllers[slot] = nullptr;
    }
  }
}
}  // namespace

void poll_events(std::vector<InputEvent>& out) {
  scripted_events(out);
  scripted_pads();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    InputEvent ev{};
    switch (e.type) {
      case SDL_CONTROLLERDEVICEADDED:
        controller_added(e.cdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        controller_removed(e.cdevice.which);
        break;
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

// ---- Scripted input (automated tests) ----------------------------------------------------------
// SHREK2_INPUT="3600:Return,3700:Down+10,..." presses the SDL key named after the colon at that
// presented frame, holding it for 4 frames (or the +N given).
namespace {
struct ScriptedKey {
  uint64_t frame;
  SDL_Scancode sc;
  uint32_t hold;
  bool down_sent = false, up_sent = false;
};
std::vector<ScriptedKey> g_script;
std::atomic<uint64_t> g_frame{0};
bool g_script_parsed = false;

void parse_script() {
  g_script_parsed = true;
  const char* e = getenv("SHREK2_INPUT");
  if (!e) return;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    ScriptedKey k{};
    k.frame = strtoull(item.c_str(), nullptr, 10);
    std::string key = item.substr(colon + 1);
    k.hold = 4;
    size_t plus = key.find('+');
    if (plus != std::string::npos) {
      k.hold = (uint32_t)atoi(key.c_str() + plus + 1);
      key = key.substr(0, plus);
    }
    k.sc = SDL_GetScancodeFromName(key.c_str());
    if (k.sc == SDL_SCANCODE_UNKNOWN) rt::log(rt::Log::Warn, "SHREK2_INPUT: unknown key '%s'", key.c_str());
    else g_script.push_back(k);
  }
}

void scripted_events(std::vector<InputEvent>& out) {
  if (!g_script_parsed) parse_script();
  uint64_t f = g_frame.load();
  for (auto& k : g_script) {
    const KeyMap* m = lookup(k.sc);
    if (!m) continue;
    bool want_down = f >= k.frame && !k.down_sent, want_up = f >= k.frame + k.hold && k.down_sent && !k.up_sent;
    if (!want_down && !want_up) continue;
    InputEvent ev{};
    ev.kind = want_down ? InputEvent::KeyDown : InputEvent::KeyUp;
    ev.vk = m->vk;
    ev.scancode = m->dik;
    g_keys[dik_index(m->dik)] = want_down ? 0x80 : 0;
    out.push_back(ev);
    if (want_down) {
      k.down_sent = true;
      rt::log(rt::Log::Info, "scripted input: %s down at frame %llu", SDL_GetScancodeName(k.sc), (unsigned long long)f);
    } else {
      k.up_sent = true;
    }
  }
}

// SHREK2_PAD="600:p0.ly=-1+90,700:p0.a,..." drives virtual gamepads by presented frame:
// pN.<button>[+hold] presses a button (a b x y lb rb lt rt back start ls rs up down left right),
// pN.<axis>=v[+hold] holds a stick axis (lx ly rx ry, -1..1, +y down) or trigger (lt rt, 0..1),
// pN.connect / pN.disconnect plug the pad in or out. A pad named in the script starts connected.
struct ScriptedPad {
  uint64_t frame;
  int slot;
  std::string control;
  float value;
  uint32_t hold;
  bool started = false, ended = false;
};
std::vector<ScriptedPad> g_pad_script;
GamepadState g_scripted[kMaxGamepads];
bool g_pad_scripted[kMaxGamepads] = {};
bool g_pad_script_parsed = false;

uint32_t pad_button(const std::string& n) {
  static const std::pair<const char*, uint32_t> names[] = {
      {"a", PadA}, {"b", PadB}, {"x", PadX}, {"y", PadY}, {"lb", PadLB}, {"rb", PadRB}, {"back", PadBack},
      {"start", PadStart}, {"ls", PadLS}, {"rs", PadRS}, {"up", PadUp}, {"down", PadDown}, {"left", PadLeft},
      {"right", PadRight}};
  for (auto& [name, bit] : names)
    if (n == name) return bit;
  return 0;
}

float* pad_axis(GamepadState& p, const std::string& n) {
  if (n == "lx") return &p.lx;
  if (n == "ly") return &p.ly;
  if (n == "rx") return &p.rx;
  if (n == "ry") return &p.ry;
  if (n == "lt") return &p.lt;
  if (n == "rt") return &p.rt;
  return nullptr;
}

void parse_pad_script() {
  g_pad_script_parsed = true;
  const char* e = getenv("SHREK2_PAD");
  if (!e) return;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    ScriptedPad k{};
    size_t colon = item.find(':'), dot = item.find('.');
    if (colon == std::string::npos || dot == std::string::npos || dot < colon + 3 || item[colon + 1] != 'p') {
      rt::log(rt::Log::Warn, "SHREK2_PAD: cannot parse '%s'", item.c_str());
      continue;
    }
    k.frame = strtoull(item.c_str(), nullptr, 10);
    k.slot = atoi(item.c_str() + colon + 2);
    std::string rest = item.substr(dot + 1);
    k.hold = 4;
    k.value = 1;
    if (size_t plus = rest.find('+'); plus != std::string::npos) {
      k.hold = (uint32_t)atoi(rest.c_str() + plus + 1);
      rest = rest.substr(0, plus);
    }
    if (size_t eq = rest.find('='); eq != std::string::npos) {
      k.value = strtof(rest.c_str() + eq + 1, nullptr);
      rest = rest.substr(0, eq);
    }
    k.control = rest;
    if (k.slot < 0 || k.slot >= kMaxGamepads) continue;
    GamepadState probe;
    if (!pad_button(rest) && !pad_axis(probe, rest) && rest != "connect" && rest != "disconnect") {
      rt::log(rt::Log::Warn, "SHREK2_PAD: unknown control '%s'", rest.c_str());
      continue;
    }
    g_pad_script.push_back(k);
    g_pad_scripted[k.slot] = true;
    g_scripted[k.slot].connected = true;
  }
}

void scripted_pads() {
  if (!g_pad_script_parsed) parse_pad_script();
  uint64_t f = g_frame.load();
  for (auto& k : g_pad_script) {
    GamepadState& p = g_scripted[k.slot];
    if (!k.started && f >= k.frame) {
      k.started = true;
      if (k.control == "connect" || k.control == "disconnect") {
        p = GamepadState{};
        p.connected = k.control == "connect";
        k.ended = true;
      } else if (uint32_t bit = pad_button(k.control)) {
        p.buttons |= bit;
      } else {
        *pad_axis(p, k.control) = k.value;
      }
      rt::log(rt::Log::Info, "scripted pad %d: %s=%g at frame %llu", k.slot, k.control.c_str(), k.value, (unsigned long long)f);
    } else if (k.started && !k.ended && f >= k.frame + k.hold) {
      k.ended = true;
      if (uint32_t bit = pad_button(k.control)) p.buttons &= ~bit;
      else *pad_axis(p, k.control) = 0;
    }
  }
}
}  // namespace

void frame_presented(uint64_t frame) { g_frame = frame; }

GamepadState gamepad_state(int slot) {
  GamepadState p;
  if (slot < 0 || slot >= kMaxGamepads) return p;
  if (g_pad_scripted[slot]) return g_scripted[slot];
  SDL_GameController* c = g_controllers[slot];
  if (!c || !SDL_GameControllerGetAttached(c)) return p;
  p.connected = true;
  static const std::pair<SDL_GameControllerButton, uint32_t> buttons[] = {
      {SDL_CONTROLLER_BUTTON_A, PadA}, {SDL_CONTROLLER_BUTTON_B, PadB}, {SDL_CONTROLLER_BUTTON_X, PadX},
      {SDL_CONTROLLER_BUTTON_Y, PadY}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PadLB},
      {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PadRB}, {SDL_CONTROLLER_BUTTON_BACK, PadBack},
      {SDL_CONTROLLER_BUTTON_START, PadStart}, {SDL_CONTROLLER_BUTTON_LEFTSTICK, PadLS},
      {SDL_CONTROLLER_BUTTON_RIGHTSTICK, PadRS}, {SDL_CONTROLLER_BUTTON_DPAD_UP, PadUp},
      {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PadDown}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PadLeft},
      {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PadRight}};
  for (auto& [b, bit] : buttons)
    if (SDL_GameControllerGetButton(c, b)) p.buttons |= bit;
  auto axis = [c](SDL_GameControllerAxis a) { return std::max(-1.f, SDL_GameControllerGetAxis(c, a) / 32767.f); };
  p.lx = axis(SDL_CONTROLLER_AXIS_LEFTX), p.ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
  p.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX), p.ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
  p.lt = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT), p.rt = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  if (p.lt > 0.5f) p.buttons |= PadLT;
  if (p.rt > 0.5f) p.buttons |= PadRT;
  return p;
}

void gamepad_rumble(int slot, float strong, float weak) {
  if (slot < 0 || slot >= kMaxGamepads) return;
  if (g_pad_scripted[slot]) {
    rt::log(rt::Log::Info, "scripted pad %d: rumble strong=%.3f weak=%.3f", slot, strong, weak);
    return;
  }
  // The game keeps an effect playing until it stops it; SDL needs a duration.
  if (SDL_GameController* c = g_controllers[slot])
    SDL_GameControllerRumble(c, (Uint16)(std::clamp(strong, 0.f, 1.f) * 65535), (Uint16)(std::clamp(weak, 0.f, 1.f) * 65535), 10000);
}

MouseState take_mouse_state() {
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  MouseState m = g_mouse;
  g_mouse.dx = g_mouse.dy = g_mouse.wheel = 0;
  return m;
}

}  // namespace plat
