// Minimal COM support for HLE interfaces handed to the game (Direct3D 9, DirectSound, DirectInput).
//
// A guest-visible COM pointer points at 8 bytes of guest memory: [vtable][object id]. Every vtable
// slot is an HLE thunk; calls are stdcall with `this` as the first argument.
#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/rt.h"

namespace rt {

struct ComObject {
  std::atomic<int32_t> refs{1};
  std::vector<uint32_t> wrappers;  // guest pointers (one per interface) referring to this object
  // Unregisters the guest wrappers: later calls through stale guest pointers fail loudly.
  virtual ~ComObject();
  virtual const char* type_name() const = 0;
};

// Method implementation: `self` is the object, arguments start at arg(c, 1); returns EAX.
using ComFn = uint32_t (*)(Cpu* c, ComObject* self);

struct ComMethod {
  const char* name;
  int nargs;  // excluding `this`
  ComFn fn;   // nullptr = not implemented (traps with the method name)
};

class ComInterface {
 public:
  // `serialize`, when given, is held for the duration of every method call.
  ComInterface(const char* name, std::vector<ComMethod> methods, std::recursive_mutex* serialize = nullptr);
  uint32_t vtable();  // created lazily (thunks need the runtime initialised)
  const std::string& name() const { return name_; }
  void set_serialize(std::recursive_mutex* m) { serialize_ = m; }

 private:
  std::string name_;
  std::vector<ComMethod> methods_;
  std::recursive_mutex* serialize_ = nullptr;
  uint32_t vtable_ = 0;
};

// Expose `obj` to the guest through `iface`; returns the guest COM pointer.
uint32_t com_wrap(ComObject* obj, ComInterface& iface);
ComObject* com_object(uint32_t guest_ptr);
template <typename T>
T* com_as(uint32_t guest_ptr) {
  return dynamic_cast<T*>(com_object(guest_ptr));
}

// Standard IUnknown implementations (for interfaces with no extra QueryInterface support).
uint32_t com_AddRef(Cpu* c, ComObject* self);
uint32_t com_Release(Cpu* c, ComObject* self);
uint32_t com_QueryInterface_none(Cpu* c, ComObject* self);

struct Guid {
  uint32_t d1;
  uint16_t d2, d3;
  uint8_t d4[8];
};
Guid read_guid(uint32_t addr);
bool guid_eq(const Guid& a, const Guid& b);
std::string guid_str(const Guid& g);

constexpr uint32_t S_OK = 0, S_FALSE = 1;
constexpr uint32_t E_NOINTERFACE = 0x80004002, E_POINTER = 0x80004003, E_FAIL = 0x80004005,
                   E_INVALIDARG = 0x80070057, E_OUTOFMEMORY = 0x8007000e, E_NOTIMPL = 0x80004001;

}  // namespace rt
