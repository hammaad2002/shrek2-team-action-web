// dinput8.dll: IDirectInput8A and IDirectInputDevice8A for the system keyboard and mouse and four
// virtual gamepads (one per player), fed by the platform layer, plus IDirectInputEffect for rumble.
//
// The game (input setup at 0x592140) only opens a gamepad whose instance GUID matches a player's
// `pad<N>` registry value, so the four virtual pads are always enumerated with fixed GUIDs and the
// settings page writes those GUIDs. A pad with no physical controller behind it reports itself
// unplugged (Poll/Acquire fail), which the game treats as a removed controller.
#include <algorithm>
#include <cmath>
#include <mutex>

#include "../core/objects.h"
#include "../platform/platform.h"
#include "com.h"

using namespace rt;

namespace {

const Guid GUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_XAxis = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_YAxis = {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_ZAxis = {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_RxAxis = {0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_RyAxis = {0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_Button = {0xA36D02F0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_POV = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
// Product GUID in DirectInput's {PIDVID-0000-0000-0000-"PIDVID"} form (an Xbox 360 pad's IDs).
const Guid kPadProduct = {0x028E045E, 0x0000, 0x0000, {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};

// Virtual pad N: {53324750-4144-0001-8000-00000000000N} (web/src/settings.ts writes the same).
Guid pad_guid(int n) { return {0x53324750, 0x4144, 0x0001, {0x80, 0, 0, 0, 0, 0, 0, (uint8_t)n}}; }

constexpr uint32_t DI8DEVCLASS_ALL = 0, DI8DEVCLASS_POINTER = 2, DI8DEVCLASS_KEYBOARD = 3, DI8DEVCLASS_GAMECTRL = 4;
constexpr uint32_t DI8DEVTYPE_MOUSE = 0x12, DI8DEVTYPE_KEYBOARD = 0x13;
constexpr uint32_t kPadDevType = 0x00010215;  // DI8DEVTYPE_GAMEPAD, standard subtype, HID
constexpr uint32_t DIEDFL_FORCEFEEDBACK = 0x100;
constexpr uint32_t DIERR_INVALIDPARAM = 0x80070057, DIERR_NOTACQUIRED = 0x8007000c, DIERR_INPUTLOST = 0x8007001e,
                   DIERR_DEVICENOTREG = 0x80040154, DIERR_OBJECTNOTFOUND = 0x80070002, DIERR_UNSUPPORTED = 0x80004001,
                   DIERR_UNPLUGGED = 0x80040209;
constexpr uint32_t DI_OK = 0, DIENUM_CONTINUE = 1;
constexpr uint32_t DIDFT_ABSAXIS = 0x2, DIDFT_AXIS = 0x3, DIDFT_PSHBUTTON = 0x4, DIDFT_BUTTON = 0xc, DIDFT_POV = 0x10,
                   DIDFT_FFACTUATOR = 0x01000000;
constexpr uint32_t DIPROP_RANGE = 4, DIPROP_DEADZONE = 5, DIPROP_SATURATION = 6, DIPROP_FFGAIN = 7, DIPROP_AUTOCENTER = 9;
constexpr uint32_t DIPH_DEVICE = 0, DIPH_BYOFFSET = 1, DIPH_BYID = 2;

// The virtual pad: an Xbox-style controller. The D-pad is the POV hat and also drives the left
// stick (the game's menus and movement only read the sticks); LT/RT are the Z axis and buttons.
struct AxisInfo {
  const Guid* type;
  uint32_t ofs;
  const char* name;
  uint16_t usage;
  bool ff;
};
const AxisInfo kAxes[] = {{&GUID_XAxis, 0, "X Axis", 0x30, true},    {&GUID_YAxis, 4, "Y Axis", 0x31, true},
                          {&GUID_ZAxis, 8, "Z Axis", 0x32, false},   {&GUID_RxAxis, 12, "X Rotation", 0x33, false},
                          {&GUID_RyAxis, 16, "Y Rotation", 0x34, false}};
constexpr int kAxisCount = 5;
// DirectInput button n <- platform button (the order Windows gives XInput pads, then LT, RT).
const uint32_t kButtons[] = {plat::PadA,    plat::PadB,     plat::PadX,  plat::PadY,  plat::PadLB, plat::PadRB,
                             plat::PadBack, plat::PadStart, plat::PadLS, plat::PadRS, plat::PadLT, plat::PadRT};
constexpr int kButtonCount = 12;
constexpr uint32_t kFFResolution = 10000;  // reported per FF axis; the game scales rumble by it
constexpr float kStickDeadzone = 0.2f;

enum class Kind { Keyboard, Mouse, Pad };

class Device : public ComObject {
 public:
  const char* type_name() const override { return "DirectInputDevice8"; }
  Kind kind;
  int pad = -1;
  uint32_t data_size = 0;
  bool acquired = false;
  uint8_t prev_keys[256] = {};
  uint32_t sequence = 0;
  int32_t range_min[kAxisCount], range_max[kAxisCount];
  uint32_t deadzone[kAxisCount] = {};
  bool was_connected = false;
  Device() {
    std::fill(std::begin(range_min), std::end(range_min), 0);
    std::fill(std::begin(range_max), std::end(range_max), 65535);
  }
};

class DirectInput : public ComObject {
 public:
  const char* type_name() const override { return "DirectInput8"; }
};

class Effect : public ComObject {
 public:
  const char* type_name() const override { return "DirectInputEffect"; }
  int pad = -1;
  bool playing = false;
  int32_t magnitude = 0, dir[2] = {0, 0};
  uint32_t axes = 0, gain = 10000;
  ~Effect() override {
    if (playing) plat::gamepad_rumble(pad, 0, 0);
  }
  void apply() {
    float strong = 0, weak = 0;
    if (playing && magnitude != 0) {
      float g = std::min(gain, 10000u) / 10000.f;
      if (axes >= 2) {
        // The game (0x5918f0) sends motor speeds m1, m2 (0..65535) as the cartesian direction
        // (m * resolution / 65536) of a constant force.
        strong = std::fabs((float)dir[0]) / kFFResolution * g;
        weak = std::fabs((float)dir[1]) / kFFResolution * g;
      } else {
        strong = weak = std::fabs((float)magnitude) / 10000.f * g;
      }
    }
    plat::gamepad_rumble(pad, std::min(strong, 1.f), std::min(weak, 1.f));
  }
};

ComInterface& device_iface();
ComInterface& effect_iface();

uint32_t ok(Cpu*, ComObject*) { return DI_OK; }

void write_instance(uint32_t p, const Guid& guid, const Guid& product, uint32_t devtype, const char* name) {
  uint32_t size = ld32(p) ? ld32(p) : 580;
  memset(host(p + 4), 0, size - 4);
  memcpy(host(p + 4), &guid, 16);      // guidInstance
  memcpy(host(p + 20), &product, 16);  // guidProduct
  st32(p + 36, devtype);
  write_cstr(p + 40, name);            // tszInstanceName
  write_cstr(p + 300, name);           // tszProductName
  if (size >= 580 && devtype == kPadDevType) {
    st16(p + 576, 1);  // wUsagePage: generic desktop
    st16(p + 578, 5);  // wUsage: game pad
  }
}

std::string pad_name(int n) { return "Gamepad " + std::to_string(n + 1); }

// ---- IDirectInput8A --------------------------------------------------------------------------

uint32_t di_CreateDevice(Cpu* c, ComObject*) {
  Guid g = read_guid(arg(c, 1));
  auto* d = new Device();
  if (guid_eq(g, GUID_SysKeyboard)) d->kind = Kind::Keyboard;
  else if (guid_eq(g, GUID_SysMouse)) d->kind = Kind::Mouse;
  else {
    for (int n = 0; n < plat::kMaxGamepads; n++)
      if (guid_eq(g, pad_guid(n))) d->kind = Kind::Pad, d->pad = n;
    if (d->pad < 0) {
      delete d;
      log(Log::Warn, "DirectInput8::CreateDevice(%s): unknown device", guid_str(g).c_str());
      return DIERR_DEVICENOTREG;
    }
    if (trace_enabled("input")) log(Log::Info, "input: game opened virtual gamepad %d", d->pad);
  }
  st32(arg(c, 2), com_wrap(d, device_iface()));
  return DI_OK;
}

uint32_t di_EnumDevices(Cpu* c, ComObject*) {
  uint32_t type = arg(c, 1), cb = arg(c, 2), ref = arg(c, 3), flags = arg(c, 4);
  uint32_t inst = heap_alloc(process_heap(), 580, true);
  struct D {
    uint32_t cls;
    Guid g;
    Guid product;
    uint32_t devtype;
    std::string name;
  };
  std::vector<D> devs = {{DI8DEVCLASS_KEYBOARD, GUID_SysKeyboard, GUID_SysKeyboard, DI8DEVTYPE_KEYBOARD, "Keyboard"},
                         {DI8DEVCLASS_POINTER, GUID_SysMouse, GUID_SysMouse, DI8DEVTYPE_MOUSE, "Mouse"}};
  // All four pads are always listed (DIEDFL_ATTACHEDONLY included): the game enumerates only once,
  // at start-up, and a controller plugged in later must still work.
  for (int n = 0; n < plat::kMaxGamepads; n++) devs.push_back({DI8DEVCLASS_GAMECTRL, pad_guid(n), kPadProduct, kPadDevType, pad_name(n)});
  for (auto& d : devs) {
    if (type != DI8DEVCLASS_ALL && type != d.cls) continue;
    if ((flags & DIEDFL_FORCEFEEDBACK) && d.cls != DI8DEVCLASS_GAMECTRL) continue;
    st32(inst, 580);
    write_instance(inst, d.g, d.product, d.devtype, d.name.c_str());
    if (guest_call(c, cb, {inst, ref}) != DIENUM_CONTINUE) break;
  }
  heap_free(process_heap(), inst);
  return DI_OK;
}

ComInterface& dinput_iface() {
  static ComInterface i("IDirectInput8A", {{"QueryInterface", 2, com_QueryInterface_none},
                                           {"AddRef", 0, com_AddRef},
                                           {"Release", 0, com_Release},
                                           {"CreateDevice", 3, di_CreateDevice},
                                           {"EnumDevices", 4, di_EnumDevices},
                                           {"GetDeviceStatus", 1, ok},
                                           {"RunControlPanel", 2, ok},
                                           {"Initialize", 2, ok},
                                           {"FindDevice", 3, nullptr},
                                           {"EnumDevicesBySemantics", 5, nullptr},
                                           {"ConfigureDevices", 4, nullptr}});
  return i;
}

// ---- IDirectInputDevice8A --------------------------------------------------------------------

Device* dev(ComObject* o) { return static_cast<Device*>(o); }

// The pad's connection state; a newly unplugged pad also loses acquisition, as on Windows.
bool pad_connected(Device* d, plat::GamepadState* out = nullptr) {
  plat::GamepadState s = plat::gamepad_state(d->pad);
  if (s.connected != d->was_connected) {
    if (trace_enabled("input")) log(Log::Info, "input: virtual gamepad %d %s", d->pad, s.connected ? "connected" : "unplugged");
    d->was_connected = s.connected;
    if (!s.connected) d->acquired = false;
  }
  if (out) *out = s;
  return s.connected;
}

uint32_t dev_GetCapabilities(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t caps = arg(c, 1);
  uint32_t size = ld32(caps);
  memset(host(caps + 4), 0, size - 4);
  if (d->kind == Kind::Pad) {
    st32(caps + 4, (pad_connected(d) ? 0x1u : 0u) | 0x100u);  // DIDC_ATTACHED, DIDC_FORCEFEEDBACK
    st32(caps + 8, kPadDevType);
    st32(caps + 12, kAxisCount);
    st32(caps + 16, kButtonCount);
    st32(caps + 20, 1);  // POVs
    return DI_OK;
  }
  st32(caps + 4, 0x5);  // DIDC_ATTACHED | DIDC_EMULATED
  st32(caps + 8, d->kind == Kind::Keyboard ? DI8DEVTYPE_KEYBOARD : DI8DEVTYPE_MOUSE);
  st32(caps + 12, d->kind == Kind::Keyboard ? 0 : 3);    // axes
  st32(caps + 16, d->kind == Kind::Keyboard ? 128 : 4);  // buttons
  return DI_OK;
}

uint32_t dev_GetDeviceInfo(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t inst = arg(c, 1);
  if (d->kind == Kind::Pad) write_instance(inst, pad_guid(d->pad), kPadProduct, kPadDevType, pad_name(d->pad).c_str());
  else if (d->kind == Kind::Keyboard) write_instance(inst, GUID_SysKeyboard, GUID_SysKeyboard, DI8DEVTYPE_KEYBOARD, "Keyboard");
  else write_instance(inst, GUID_SysMouse, GUID_SysMouse, DI8DEVTYPE_MOUSE, "Mouse");
  return DI_OK;
}

// DIDEVICEOBJECTINSTANCEA for a pad axis, button or the POV hat.
void write_object(uint32_t p, const Guid& type, uint32_t ofs, uint32_t dwtype, uint32_t flags, const std::string& name,
                  uint16_t usage_page, uint16_t usage, bool ff) {
  memset(host(p), 0, 316);
  st32(p, 316);
  memcpy(host(p + 4), &type, 16);
  st32(p + 0x14, ofs);
  st32(p + 0x18, dwtype);
  st32(p + 0x1c, flags);
  write_cstr(p + 0x20, name);
  if (ff) {
    st32(p + 0x124, 10000);          // dwFFMaxForce
    st32(p + 0x128, kFFResolution);  // dwFFForceResolution
  }
  st16(p + 0x130, usage_page);
  st16(p + 0x132, usage);
}

uint32_t dev_EnumObjects(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t cb = arg(c, 1), ref = arg(c, 2), flags = arg(c, 3);
  if (d->kind != Kind::Pad) return DI_OK;
  bool all = (flags & 0xff) == 0, ff_only = flags & DIDFT_FFACTUATOR;
  uint32_t obj = heap_alloc(process_heap(), 316, true);
  bool go = true;
  if (all || (flags & DIDFT_AXIS))
    for (int n = 0; go && n < kAxisCount; n++) {
      const AxisInfo& a = kAxes[n];
      if (ff_only && !a.ff) continue;
      write_object(obj, *a.type, a.ofs, DIDFT_ABSAXIS | (uint32_t)n << 8 | (a.ff ? DIDFT_FFACTUATOR : 0),
                   0x100 | (a.ff ? 1u : 0u), a.name, 1, a.usage, a.ff);  // DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR
      go = guest_call(c, cb, {obj, ref}) == DIENUM_CONTINUE;
    }
  if (!ff_only && (all || (flags & DIDFT_BUTTON)))
    for (int n = 0; go && n < kButtonCount; n++) {
      write_object(obj, GUID_Button, 0x30 + n, DIDFT_PSHBUTTON | (uint32_t)n << 8, 0, "Button " + std::to_string(n), 9, n + 1, false);
      go = guest_call(c, cb, {obj, ref}) == DIENUM_CONTINUE;
    }
  if (!ff_only && go && (all || (flags & DIDFT_POV))) {
    write_object(obj, GUID_POV, 0x20, DIDFT_POV, 0, "Hat Switch", 1, 0x39, false);
    guest_call(c, cb, {obj, ref});
  }
  heap_free(process_heap(), obj);
  return DI_OK;
}

// The pad axes a property header (DIPROPHEADER) addresses.
std::vector<int> prop_axes(uint32_t hdr) {
  uint32_t obj = ld32(hdr + 8), how = ld32(hdr + 12);
  std::vector<int> axes;
  for (int n = 0; n < kAxisCount; n++) {
    if (how == DIPH_DEVICE || (how == DIPH_BYOFFSET && obj == kAxes[n].ofs) ||
        (how == DIPH_BYID && (obj & DIDFT_AXIS) && ((obj >> 8) & 0xffff) == (uint32_t)n))
      axes.push_back(n);
  }
  return axes;
}

uint32_t dev_SetProperty(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t prop = arg(c, 1), hdr = arg(c, 2);
  if (d->kind != Kind::Pad) return DI_OK;
  if (prop != DIPROP_RANGE && prop != DIPROP_DEADZONE && prop != DIPROP_SATURATION) return DI_OK;  // gain, autocenter, ...
  std::vector<int> axes = prop_axes(hdr);
  if (axes.empty()) return DIERR_OBJECTNOTFOUND;
  for (int n : axes) {
    if (prop == DIPROP_RANGE) {
      int32_t lo = (int32_t)ld32(hdr + 16), hi = (int32_t)ld32(hdr + 20);
      if (lo >= hi) return DIERR_INVALIDPARAM;
      d->range_min[n] = lo, d->range_max[n] = hi;
    } else if (prop == DIPROP_DEADZONE) {
      d->deadzone[n] = std::min(ld32(hdr + 16), 10000u);
    }
  }
  return DI_OK;
}

uint32_t dev_GetProperty(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t prop = arg(c, 1), hdr = arg(c, 2);
  if (d->kind != Kind::Pad) return DIERR_UNSUPPORTED;
  if (prop == DIPROP_FFGAIN || prop == DIPROP_SATURATION || prop == DIPROP_AUTOCENTER) {
    st32(hdr + 16, prop == DIPROP_AUTOCENTER ? 1 : 10000);
    return DI_OK;
  }
  if (prop != DIPROP_RANGE && prop != DIPROP_DEADZONE) return DIERR_UNSUPPORTED;
  std::vector<int> axes = prop_axes(hdr);
  if (axes.empty() || ld32(hdr + 12) == DIPH_DEVICE) return DIERR_INVALIDPARAM;
  int n = axes[0];
  if (prop == DIPROP_RANGE) {
    st32(hdr + 16, (uint32_t)d->range_min[n]);
    st32(hdr + 20, (uint32_t)d->range_max[n]);
  } else {
    st32(hdr + 16, d->deadzone[n]);
  }
  return DI_OK;
}

uint32_t dev_SetDataFormat(Cpu* c, ComObject* self) {
  uint32_t fmt = arg(c, 1);
  dev(self)->data_size = ld32(fmt + 12);  // DIDATAFORMAT.dwDataSize
  return DI_OK;
}

uint32_t dev_Acquire(Cpu*, ComObject* self) {
  auto* d = dev(self);
  if (d->kind == Kind::Pad && !pad_connected(d)) return DIERR_UNPLUGGED;
  d->acquired = true;
  return DI_OK;
}
uint32_t dev_Unacquire(Cpu*, ComObject* self) {
  dev(self)->acquired = false;
  return DI_OK;
}

uint32_t dev_Poll(Cpu*, ComObject* self) {
  auto* d = dev(self);
  if (d->kind != Kind::Pad) return DI_OK;
  if (!pad_connected(d)) return DIERR_INPUTLOST;
  return d->acquired ? DI_OK : DIERR_NOTACQUIRED;
}

// Radial dead zone, rescaled so the usable range still reaches full deflection.
void deadzone(float& x, float& y) {
  float m = std::sqrt(x * x + y * y);
  if (m <= kStickDeadzone) {
    x = y = 0;
    return;
  }
  float scale = std::min(1.f, (m - kStickDeadzone) / (1 - kStickDeadzone)) / m;
  x *= scale, y *= scale;
}

void fill_joystate(Device* d, const plat::GamepadState& s, uint32_t out, uint32_t size) {
  float lx = s.lx, ly = s.ly, rx = s.rx, ry = s.ry;
  deadzone(lx, ly);
  deadzone(rx, ry);
  int dx = (s.buttons & plat::PadRight ? 1 : 0) - (s.buttons & plat::PadLeft ? 1 : 0);
  int dy = (s.buttons & plat::PadDown ? 1 : 0) - (s.buttons & plat::PadUp ? 1 : 0);
  if (dx || dy) lx = (float)dx, ly = (float)dy;
  const float v[kAxisCount] = {lx, ly, std::clamp(s.lt - s.rt, -1.f, 1.f), rx, ry};
  uint8_t st[0x110] = {};
  for (int n = 0; n < kAxisCount; n++) {
    float lo = (float)d->range_min[n], hi = (float)d->range_max[n];
    int32_t value = (int32_t)std::lround(lo + (std::clamp(v[n], -1.f, 1.f) + 1) * 0.5f * (hi - lo));
    memcpy(st + kAxes[n].ofs, &value, 4);
  }
  uint32_t pov = 0xffffffff;  // centred; otherwise hundredths of a degree clockwise from up
  if (dx || dy) pov = (uint32_t)std::lround(std::fmod(std::atan2((double)dx, (double)-dy) * 18000 / 3.14159265358979 + 36000, 36000));
  memcpy(st + 0x20, &pov, 4);
  for (int k = 1; k < 4; k++) memset(st + 0x20 + 4 * k, 0xff, 4);
  for (int n = 0; n < kButtonCount; n++) st[0x30 + n] = (s.buttons & kButtons[n]) ? 0x80 : 0;
  memcpy(host(out), st, std::min<uint32_t>(size, sizeof st));
}

uint32_t dev_GetDeviceState(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t size = arg(c, 1), out = arg(c, 2);
  memset(host(out), 0, size);
  if (d->kind == Kind::Keyboard) {
    memcpy(host(out), plat::keyboard_state(), std::min<uint32_t>(size, 256));
  } else if (d->kind == Kind::Mouse) {
    plat::MouseState m = plat::take_mouse_state();
    st32(out + 0, (uint32_t)m.dx);
    st32(out + 4, (uint32_t)m.dy);
    st32(out + 8, (uint32_t)m.wheel);
    for (uint32_t b = 0; b < 4 && 12 + b < size; b++) st8(out + 12 + b, m.buttons[b]);
  } else {
    plat::GamepadState s;
    if (!pad_connected(d, &s)) return DIERR_INPUTLOST;
    if (!d->acquired) return DIERR_NOTACQUIRED;
    fill_joystate(d, s, out, size);
  }
  return DI_OK;
}

// Buffered keyboard data: report key transitions since the last call.
uint32_t dev_GetDeviceData(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t objsize = arg(c, 1), out = arg(c, 2), inout = arg(c, 3), flags = arg(c, 4);
  uint32_t cap = ld32(inout), n = 0;
  if (d->kind == Kind::Keyboard) {
    const uint8_t* keys = plat::keyboard_state();
    for (uint32_t k = 0; k < 256 && n < cap; k++) {
      if ((keys[k] & 0x80) == (d->prev_keys[k] & 0x80)) continue;
      if (out) {
        uint32_t e = out + n * objsize;
        st32(e + 0, k);                 // dwOfs (DIK code)
        st32(e + 4, keys[k] & 0x80);    // dwData
        st32(e + 8, 0);                 // dwTimeStamp
        st32(e + 12, d->sequence++);    // dwSequence
        if (objsize >= 20) st32(e + 16, 0);
      }
      if (!(flags & 1)) d->prev_keys[k] = keys[k];  // DIGDD_PEEK leaves the buffer intact
      n++;
    }
  }
  st32(inout, n);
  return DI_OK;
}

uint32_t dev_CreateEffect(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t out = arg(c, 3);
  if (d->kind != Kind::Pad) return DIERR_UNSUPPORTED;
  auto* e = new Effect();
  e->pad = d->pad;
  if (uint32_t p = arg(c, 2)) e->gain = ld32(p + 0x10);  // DIEFFECT.dwGain
  st32(out, com_wrap(e, effect_iface()));
  if (trace_enabled("input")) log(Log::Info, "input: rumble effect created for virtual gamepad %d", d->pad);
  return DI_OK;
}

ComInterface& device_iface() {
  static ComInterface i("IDirectInputDevice8A", {{"QueryInterface", 2, com_QueryInterface_none},
                                                 {"AddRef", 0, com_AddRef},
                                                 {"Release", 0, com_Release},
                                                 {"GetCapabilities", 1, dev_GetCapabilities},
                                                 {"EnumObjects", 3, dev_EnumObjects},
                                                 {"GetProperty", 2, dev_GetProperty},
                                                 {"SetProperty", 2, dev_SetProperty},
                                                 {"Acquire", 0, dev_Acquire},
                                                 {"Unacquire", 0, dev_Unacquire},
                                                 {"GetDeviceState", 2, dev_GetDeviceState},
                                                 {"GetDeviceData", 4, dev_GetDeviceData},
                                                 {"SetDataFormat", 1, dev_SetDataFormat},
                                                 {"SetEventNotification", 1, ok},
                                                 {"SetCooperativeLevel", 2, ok},
                                                 {"GetObjectInfo", 3, nullptr},
                                                 {"GetDeviceInfo", 1, dev_GetDeviceInfo},
                                                 {"RunControlPanel", 2, ok},
                                                 {"Initialize", 3, ok},
                                                 {"CreateEffect", 4, dev_CreateEffect},
                                                 {"EnumEffects", 3, ok},
                                                 {"GetEffectInfo", 2, nullptr},
                                                 {"GetForceFeedbackState", 1, nullptr},
                                                 {"SendForceFeedbackCommand", 1, ok},
                                                 {"EnumCreatedEffectObjects", 3, ok},
                                                 {"Escape", 1, nullptr},
                                                 {"Poll", 0, dev_Poll},
                                                 {"SendDeviceData", 4, nullptr},
                                                 {"EnumEffectsInFile", 4, nullptr},
                                                 {"WriteEffectToFile", 4, nullptr},
                                                 {"BuildActionMap", 3, nullptr},
                                                 {"SetActionMap", 3, nullptr},
                                                 {"GetImageInfo", 1, nullptr}});
  return i;
}

// ---- IDirectInputEffect (a constant force played as rumble) -----------------------------------

Effect* eff(ComObject* o) { return static_cast<Effect*>(o); }

constexpr uint32_t DIEP_GAIN = 0x4, DIEP_AXES = 0x20, DIEP_DIRECTION = 0x40,
                   DIEP_TYPESPECIFICPARAMS = 0x100, DIEP_START = 0x20000000;

uint32_t eff_SetParameters(Cpu* c, ComObject* self) {
  auto* e = eff(self);
  uint32_t p = arg(c, 1), flags = arg(c, 2);
  // DIEFFECT: cAxes +0x1c, rglDirection +0x24, cbTypeSpecificParams +0x2c, lpvTypeSpecificParams +0x30.
  if (flags & (DIEP_AXES | DIEP_DIRECTION)) e->axes = ld32(p + 0x1c);
  if ((flags & DIEP_DIRECTION) && ld32(p + 0x24))
    for (uint32_t k = 0; k < std::min(e->axes, 2u); k++) e->dir[k] = (int32_t)ld32(ld32(p + 0x24) + 4 * k);
  if ((flags & DIEP_TYPESPECIFICPARAMS) && ld32(p + 0x2c) >= 4 && ld32(p + 0x30)) e->magnitude = (int32_t)ld32(ld32(p + 0x30));
  if (flags & DIEP_GAIN) e->gain = ld32(p + 0x10);
  if (flags & DIEP_START) e->playing = true;
  if (e->playing) e->apply();
  return DI_OK;
}

uint32_t eff_Start(Cpu*, ComObject* self) {
  eff(self)->playing = true;
  eff(self)->apply();
  return DI_OK;
}

uint32_t eff_Stop(Cpu*, ComObject* self) {
  auto* e = eff(self);
  if (e->playing) {
    e->playing = false;
    e->apply();
  }
  return DI_OK;
}

uint32_t eff_GetEffectStatus(Cpu* c, ComObject* self) {
  if (arg(c, 1)) st32(arg(c, 1), eff(self)->playing ? 1 : 0);  // DIEGES_PLAYING
  return DI_OK;
}

ComInterface& effect_iface() {
  static ComInterface i("IDirectInputEffect", {{"QueryInterface", 2, com_QueryInterface_none},
                                               {"AddRef", 0, com_AddRef},
                                               {"Release", 0, com_Release},
                                               {"Initialize", 3, ok},
                                               {"GetEffectGuid", 1, nullptr},
                                               {"GetParameters", 2, nullptr},
                                               {"SetParameters", 2, eff_SetParameters},
                                               {"Start", 2, eff_Start},
                                               {"Stop", 0, eff_Stop},
                                               {"GetEffectStatus", 1, eff_GetEffectStatus},
                                               {"Download", 0, ok},
                                               {"Unload", 0, ok},
                                               {"Escape", 1, nullptr}});
  return i;
}

}  // namespace

HLE_STDCALL("dinput8.dll", DirectInput8Create, 5) {
  st32(arg(c, 3), com_wrap(new DirectInput(), dinput_iface()));
  return DI_OK;
}
