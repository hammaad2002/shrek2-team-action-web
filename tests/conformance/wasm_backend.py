"""
Runs the recompiler's WebAssembly backend output (work/gen-wasm/game.wasm) in-process with
wasmtime, behind the same interface funcs.py uses for the native C-backend library:
test_mem() / test_run_ms() / test_trap_msg().

The runtime helpers the module imports (x87, rep movs/stos, cpuid, ...) come from
runtime/src/cpu_helpers.c compiled with GUEST_BASE bound to the wasm linear memory
(work/conf/libhelpers_wasm.so, built by build.sh), so both backends share one implementation.
"""
import ctypes
import json

import wasmtime as w

# Guest addresses used by the harness (outside the image, scratch and stack regions).
CPU_ADDR = 0x0FFF0000
INDEX_ADDR = 0x10000000
MEM_PAGES = 0x3100  # covers the stack window at 0x30000000

I32, I64, F64 = w.ValType.i32(), w.ValType.i64(), w.ValType.f64()
SIG = {"i32": I32, "i64": I64, "f64": F64}

# Must match RUNTIME_IMPORTS in recomp/src/emit-wasm.ts.
IMPORTS = [
    ("rt_hle", ["i32", "i32"], []),
    ("rt_dispatch", ["i32", "i32"], []),
    ("rt_trap_id", ["i32", "i32"], []),
    ("rt_cpuid", ["i32"], []),
    ("rt_rep_movs", ["i32", "i32", "i32"], []),
    ("rt_rep_stos", ["i32", "i32", "i32"], []),
    ("rt_rcl", ["i32", "i32", "i32", "i32"], ["i64"]),
    ("rt_rcr", ["i32", "i32", "i32", "i32"], ["i64"]),
    ("rt_f80_load", ["i32"], ["f64"]),
    ("rt_f80_store", ["i32", "i32", "f64"], []),
    ("rt_fist16", ["f64", "i32"], ["i32"]),
    ("rt_fist32", ["f64", "i32"], ["i32"]),
    ("rt_fist64", ["f64", "i32"], ["i64"]),
    ("rt_frndint", ["f64", "i32"], ["f64"]),
    ("rt_fxam", ["f64"], ["i32"]),
    ("rt_sin", ["f64"], ["f64"]),
    ("rt_cos", ["f64"], ["f64"]),
    ("rt_tan", ["f64"], ["f64"]),
    ("rt_atan2", ["f64", "f64"], ["f64"]),
    ("rt_log2", ["f64"], ["f64"]),
    ("rt_f2xm1", ["f64"], ["f64"]),
    ("rt_fscale", ["f64", "f64"], ["f64"]),
    ("rt_fprem", ["i32", "i32"], []),
    ("rt_fprem1", ["i32", "i32"], []),
    ("rt_fnsave", ["i32", "i32"], []),
    ("rt_fnstenv", ["i32", "i32"], []),
    ("rt_frstor", ["i32", "i32"], []),
    ("rt_fldenv", ["i32", "i32"], []),
]
CTY = {"i32": ctypes.c_uint32, "i64": ctypes.c_uint64, "f64": ctypes.c_double}
# Helpers whose i32 arguments include a Cpu* (first argument) that must become a host pointer.
TAKES_CPU = {"rt_cpuid", "rt_rep_movs", "rt_rep_stos", "rt_f80_store", "rt_fprem", "rt_fprem1",
             "rt_fnsave", "rt_fnstenv", "rt_frstor", "rt_fldenv"}


class GuestTrap(Exception):
    pass


def _leb(b, i):
    v = shift = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if not x & 0x80:
            return v, i


def custom_sections(b):
    """Custom sections of a wasm binary by name."""
    out, i = {}, 8
    while i < len(b):
        sid = b[i]
        size, j = _leb(b, i + 1)
        if sid == 0:
            n, k = _leb(b, j)
            out[b[k:k + n].decode()] = b[k + n:j + size]
        i = j + size
    return out


def _shared_as_extern(self):
    # wasmtime-py 49 wraps the shared-memory pointer twice; pass it through as-is.
    from wasmtime import _ffi as ffi
    union = ffi.wasmtime_extern_union(sharedmemory=self.ptr())
    return ffi.wasmtime_extern_t(ffi.WASMTIME_EXTERN_SHAREDMEMORY, union)


w.SharedMemory._as_extern = _shared_as_extern


class WasmLib:
    def __init__(self, wasm_path, helpers_path, cpu_type):
        self.cpu_type = cpu_type
        cfg = w.Config()
        cfg.wasm_threads = True
        cfg.wasm_bulk_memory = True
        cfg.shared_memory = True
        cfg.consume_fuel = True
        self.engine = w.Engine(cfg)
        self.store = w.Store(self.engine)
        module = w.Module.from_file(self.engine, wasm_path)
        custom = custom_sections(open(wasm_path, "rb").read())
        self.traps = json.loads(custom["shrek2.traps"].decode()) if "shrek2.traps" in custom else []
        funcs = custom.get("shrek2.funcs")
        self.mem = w.SharedMemory(self.engine, w.MemoryType(w.Limits(MEM_PAGES, 65536), shared=True))
        self.base = ctypes.cast(self.mem.data_ptr(), ctypes.c_void_p).value
        self.h = ctypes.CDLL(helpers_path)
        self.h.rt_set_guest_base(ctypes.c_uint64(self.base))
        self.trap_msg = b""
        linker = w.Linker(self.engine)
        linker.define(self.store, "env", "memory", self.mem)
        for name, params, results in IMPORTS:
            linker.define_func("env", name, w.FuncType([SIG[p] for p in params], [SIG[r] for r in results]), self._import(name, params, results))
        # u16 index over .text: guest address -> table index + 1.
        if funcs is not None:
            raw = bytes(funcs)
            n = int.from_bytes(raw[:4], "little")
            addrs = [int.from_bytes(raw[4 + 4 * i:8 + 4 * i], "little") for i in range(n)]
            lo = 0x401000
            for i, a in enumerate(addrs):
                ctypes.c_uint16.from_address(self.base + INDEX_ADDR + 2 * (a - lo)).value = i + 1
        g = w.Global(self.store, w.GlobalType(I32, False), w.Val.i32(INDEX_ADDR))
        linker.define(self.store, "env", "index_base", g)
        inst = linker.instantiate(self.store, module)
        self.dispatch = inst.exports(self.store)["dispatch"]

    def _import(self, name, params, results):
        if name == "rt_trap_id":
            def trap(c, i):
                self.trap_msg = (self.traps[i] if i < len(self.traps) else f"trap #{i}").encode()
                raise GuestTrap(self.trap_msg.decode())
            return trap
        if name in ("rt_hle", "rt_dispatch"):
            def bad(c, x):
                self.trap_msg = f"{name}({x:#x}) in a pure-function test".encode()
                raise GuestTrap(self.trap_msg.decode())
            return bad
        fn = getattr(self.h, name)
        argt = [CTY[p] for p in params]
        if name in TAKES_CPU:
            argt[0] = ctypes.c_void_p
        fn.argtypes = argt
        fn.restype = CTY[results[0]] if results else None
        base = self.base
        if name in TAKES_CPU:
            def call(c, *rest):
                r = fn(base + c, *rest)
                return r if results else None
            return call
        return lambda *a: fn(*a)

    def test_mem(self):
        return self.base

    def test_init(self):
        return 0

    def test_sizeof_cpu(self):
        return ctypes.sizeof(self.cpu_type)

    def test_trap_msg(self):
        return self.trap_msg

    def run(self, addr, c):
        n = ctypes.sizeof(c)
        ctypes.memmove(self.base + CPU_ADDR, ctypes.addressof(c), n)
        self.trap_msg = b""
        self.store.set_fuel(400_000_000)
        rc = 0
        try:
            self.dispatch(self.store, CPU_ADDR, addr)
        except w.Trap as e:
            if not self.trap_msg:
                self.trap_msg = str(e).split("\n")[0].encode()
            rc = 1
        except (w.WasmtimeError, GuestTrap) as e:
            if not self.trap_msg:
                self.trap_msg = str(e).split("\n")[0].encode()
            rc = 1
        ctypes.memmove(ctypes.addressof(c), self.base + CPU_ADDR, n)
        return rc
