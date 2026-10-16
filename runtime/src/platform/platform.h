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
// Gamepads, one per player slot (the page or SDL decides which physical pad feeds which slot).
// Buttons use the W3C "standard" Gamepad order; sticks are -1..1 with +x right, +y down.
enum PadButton : uint32_t {
  PadA = 1u << 0, PadB = 1u << 1, PadX = 1u << 2, PadY = 1u << 3, PadLB = 1u << 4, PadRB = 1u << 5,
  PadLT = 1u << 6, PadRT = 1u << 7, PadBack = 1u << 8, PadStart = 1u << 9, PadLS = 1u << 10, PadRS = 1u << 11,
  PadUp = 1u << 12, PadDown = 1u << 13, PadLeft = 1u << 14, PadRight = 1u << 15,
};
struct GamepadState {
  bool connected = false;
  uint32_t buttons = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0;  // sticks
  float lt = 0, rt = 0;                  // triggers, 0..1
};
constexpr int kMaxGamepads = 4;
GamepadState gamepad_state(int slot);
// Rumble: strong (low-frequency) and weak (high-frequency) motor strength 0..1; 0, 0 stops.
void gamepad_rumble(int slot, float strong, float weak);
// Called once per presented frame (drives scripted input: SHREK2_INPUT="frame:Key[+hold],...").
void frame_presented(uint64_t frame);
// OpenGL ES 3.0 context for the renderer (created on the calling thread).
bool gl_create_context();
void gl_swap();
void gl_set_swap_interval(int interval);
// Audio output: `render` is called from the audio thread for interleaved stereo float frames.
bool audio_open(int rate, void (*render)(float* out, int frames));

}  // namespace plat
