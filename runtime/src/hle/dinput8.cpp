// dinput8.dll: IDirectInput8A and IDirectInputDevice8A for the system keyboard and mouse (and,
// later, gamepads), fed by the platform layer.
#include <mutex>

#include "../core/objects.h"
#include "../platform/platform.h"
#include "com.h"

using namespace rt;

namespace {

const Guid GUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const Guid GUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

constexpr uint32_t DI8DEVCLASS_ALL = 0, DI8DEVCLASS_POINTER = 2, DI8DEVCLASS_KEYBOARD = 3, DI8DEVCLASS_GAMECTRL = 4;
constexpr uint32_t DI8DEVTYPE_MOUSE = 0x12, DI8DEVTYPE_KEYBOARD = 0x13;
constexpr uint32_t DIERR_INVALIDPARAM = 0x80070057, DIERR_NOTACQUIRED = 0x8007000c, DIERR_DEVICENOTREG = 0x80040154;
constexpr uint32_t DI_OK = 0, DIENUM_CONTINUE = 1;

enum class Kind { Keyboard, Mouse };

class Device : public ComObject {
 public:
  const char* type_name() const override { return "DirectInputDevice8"; }
  Kind kind;
  uint32_t data_size = 0;
  bool acquired = false;
  uint8_t prev_keys[256] = {};
  uint32_t sequence = 0;
};

class DirectInput : public ComObject {
 public:
  const char* type_name() const override { return "DirectInput8"; }
};

ComInterface& device_iface();

uint32_t ok(Cpu*, ComObject*) { return DI_OK; }

void write_instance(uint32_t p, const Guid& guid, uint32_t devtype, const char* name) {
  uint32_t size = ld32(p) ? ld32(p) : 580;
  memset(host(p + 4), 0, size - 4);
  memcpy(host(p + 4), &guid, 16);   // guidInstance
  memcpy(host(p + 20), &guid, 16);  // guidProduct
  st32(p + 36, devtype);
  write_cstr(p + 40, name);         // tszInstanceName
  write_cstr(p + 300, name);        // tszProductName
}

// ---- IDirectInput8A --------------------------------------------------------------------------

uint32_t di_CreateDevice(Cpu* c, ComObject*) {
  Guid g = read_guid(arg(c, 1));
  auto* d = new Device();
  if (guid_eq(g, GUID_SysKeyboard)) d->kind = Kind::Keyboard;
  else if (guid_eq(g, GUID_SysMouse)) d->kind = Kind::Mouse;
  else {
    delete d;
    log(Log::Warn, "DirectInput8::CreateDevice(%s): unknown device", guid_str(g).c_str());
    return DIERR_DEVICENOTREG;
  }
  st32(arg(c, 2), com_wrap(d, device_iface()));
  return DI_OK;
}

uint32_t di_EnumDevices(Cpu* c, ComObject*) {
  uint32_t type = arg(c, 1), cb = arg(c, 2), ref = arg(c, 3);
  uint32_t inst = heap_alloc(process_heap(), 580, true);
  struct D {
    uint32_t cls;
    Guid g;
    uint32_t devtype;
    const char* name;
  };
  const D devs[] = {{DI8DEVCLASS_KEYBOARD, GUID_SysKeyboard, DI8DEVTYPE_KEYBOARD, "Keyboard"},
                    {DI8DEVCLASS_POINTER, GUID_SysMouse, DI8DEVTYPE_MOUSE, "Mouse"}};
  for (auto& d : devs) {
    if (type != DI8DEVCLASS_ALL && type != d.cls) continue;
    st32(inst, 580);
    write_instance(inst, d.g, d.devtype, d.name);
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

uint32_t dev_GetCapabilities(Cpu* c, ComObject* self) {
  uint32_t caps = arg(c, 1);
  uint32_t size = ld32(caps);
  memset(host(caps + 4), 0, size - 4);
  st32(caps + 4, 0x5);  // DIDC_ATTACHED | DIDC_EMULATED
  st32(caps + 8, dev(self)->kind == Kind::Keyboard ? DI8DEVTYPE_KEYBOARD : DI8DEVTYPE_MOUSE);
  st32(caps + 12, dev(self)->kind == Kind::Keyboard ? 0 : 3);    // axes
  st32(caps + 16, dev(self)->kind == Kind::Keyboard ? 128 : 4);  // buttons
  return DI_OK;
}

uint32_t dev_SetDataFormat(Cpu* c, ComObject* self) {
  uint32_t fmt = arg(c, 1);
  dev(self)->data_size = ld32(fmt + 12);  // DIDATAFORMAT.dwDataSize
  return DI_OK;
}

uint32_t dev_Acquire(Cpu*, ComObject* self) {
  dev(self)->acquired = true;
  return DI_OK;
}
uint32_t dev_Unacquire(Cpu*, ComObject* self) {
  dev(self)->acquired = false;
  return DI_OK;
}

uint32_t dev_GetDeviceState(Cpu* c, ComObject* self) {
  auto* d = dev(self);
  uint32_t size = arg(c, 1), out = arg(c, 2);
  memset(host(out), 0, size);
  if (d->kind == Kind::Keyboard) {
    memcpy(host(out), plat::keyboard_state(), std::min<uint32_t>(size, 256));
  } else {
    plat::MouseState m = plat::take_mouse_state();
    st32(out + 0, (uint32_t)m.dx);
    st32(out + 4, (uint32_t)m.dy);
    st32(out + 8, (uint32_t)m.wheel);
    for (uint32_t b = 0; b < 4 && 12 + b < size; b++) st8(out + 12 + b, m.buttons[b]);
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

ComInterface& device_iface() {
  static ComInterface i("IDirectInputDevice8A", {{"QueryInterface", 2, com_QueryInterface_none},
                                                 {"AddRef", 0, com_AddRef},
                                                 {"Release", 0, com_Release},
                                                 {"GetCapabilities", 1, dev_GetCapabilities},
                                                 {"EnumObjects", 3, ok},
                                                 {"GetProperty", 2, nullptr},
                                                 {"SetProperty", 2, ok},
                                                 {"Acquire", 0, dev_Acquire},
                                                 {"Unacquire", 0, dev_Unacquire},
                                                 {"GetDeviceState", 2, dev_GetDeviceState},
                                                 {"GetDeviceData", 4, dev_GetDeviceData},
                                                 {"SetDataFormat", 1, dev_SetDataFormat},
                                                 {"SetEventNotification", 1, ok},
                                                 {"SetCooperativeLevel", 2, ok},
                                                 {"GetObjectInfo", 3, nullptr},
                                                 {"GetDeviceInfo", 1, nullptr},
                                                 {"RunControlPanel", 2, ok},
                                                 {"Initialize", 3, ok},
                                                 {"CreateEffect", 4, nullptr},
                                                 {"EnumEffects", 3, ok},
                                                 {"GetEffectInfo", 2, nullptr},
                                                 {"GetForceFeedbackState", 1, nullptr},
                                                 {"SendForceFeedbackCommand", 1, ok},
                                                 {"EnumCreatedEffectObjects", 3, ok},
                                                 {"Escape", 1, nullptr},
                                                 {"Poll", 0, ok},
                                                 {"SendDeviceData", 4, nullptr},
                                                 {"EnumEffectsInFile", 4, nullptr},
                                                 {"WriteEffectToFile", 4, nullptr},
                                                 {"BuildActionMap", 3, nullptr},
                                                 {"SetActionMap", 3, nullptr},
                                                 {"GetImageInfo", 1, nullptr}});
  return i;
}

}  // namespace

HLE_STDCALL("dinput8.dll", DirectInput8Create, 5) {
  st32(arg(c, 3), com_wrap(new DirectInput(), dinput_iface()));
  return DI_OK;
}
