// Host platform services: window, input events, timing. Implemented natively with SDL2 and on the
// web with Emscripten (input forwarded from the page's main thread).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plat {

struct InputEvent {
  enum Kind : uint8_t { KeyDown, KeyUp, Char, MouseMove, MouseDown, MouseUp, MouseWheel, FocusGained, FocusLost, Quit } kind;
  uint32_t vk = 0;        // Windows virtual-key code (keys)
  uint32_t scancode = 0;  // PC set-1 scan code (keys), bit 8 = extended
  uint32_t ch = 0;        // character (Char)
  int32_t x = 0, y = 0;   // client coordinates (mouse)
  int32_t dx = 0, dy = 0; // relative motion (mouse) / wheel delta
  uint32_t button = 0;    // 0 left, 1 right, 2 middle
  bool repeat = false;
};

bool init();
void create_window(const std::string& title, int width, int height);
void set_window_size(int width, int height);
void get_display_size(int* width, int* height);
void get_window_size(int* width, int* height);
void show_cursor(bool visible);
// Collect pending input events (non-blocking).
void poll_events(std::vector<InputEvent>& out);
// Keyboard state for DirectInput: 256 DIK scan codes, high bit set when down.
const uint8_t* keyboard_state();
struct MouseState { int32_t dx, dy, wheel; uint8_t buttons[4]; };
MouseState take_mouse_state();
bool has_window();

}  // namespace plat
