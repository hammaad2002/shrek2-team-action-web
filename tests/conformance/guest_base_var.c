/* Lets cpu_helpers.c operate on a guest window whose base is only known at run time (the wasm
 * linear memory in the WebAssembly-backend conformance harness). */
#include <stdint.h>
uintptr_t rt_guest_base;
void rt_set_guest_base(uint64_t base) { rt_guest_base = (uintptr_t)base; }
