"""
Whole-function differential test: run self-contained game functions in Unicorn and in the
recompiled native library from identical random states; compare registers, x87 state and memory.

  .venv/bin/python tests/conformance/funcs.py <game.exe> work/conf [vectors] [max-functions] [only-addr]

With TRACE_DIFF=1 (and work/conf/libgame_trace.so built with -DRECOMP_TRACE), the first failing
vector of each function is replayed with tracing on both sides and the first divergent block is
reported.
"""
import ctypes
import json
import math
import os
import random
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as X

EXE, CONF = sys.argv[1], sys.argv[2]
VECTORS = int(sys.argv[3]) if len(sys.argv) > 3 else 6
MAXF = int(sys.argv[4]) if len(sys.argv) > 4 else 100000
ONLY = int(sys.argv[5], 16) if len(sys.argv) > 5 else None
TRACE_DIFF = os.environ.get("TRACE_DIFF") == "1"

SCRATCH, SCRATCH_SIZE = 0x20000000, 0x100000
STACK, STACK_SIZE = 0x30000000, 0x20000
ESP0 = STACK + 0x18000
SENTINEL = 0x2FF20000
LOW = 0x0
GPRS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UC_REG = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_EBX, X.UC_X86_REG_ESP,
          X.UC_X86_REG_EBP, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI]


class Cpu(ctypes.Structure):
    _fields_ = [(g, ctypes.c_uint32) for g in GPRS] + [
        ("eflags", ctypes.c_uint32), ("df", ctypes.c_uint32), ("fs_base", ctypes.c_uint32), ("ftop", ctypes.c_uint32),
        ("fcw", ctypes.c_uint32), ("fsw", ctypes.c_uint32), ("mxcsr", ctypes.c_uint32), ("_pad0", ctypes.c_uint32),
        ("fpr", ctypes.c_double * 8), ("mm", ctypes.c_uint64 * 8), ("xmm", ctypes.c_uint32 * 32),
        ("thread_id", ctypes.c_uint32), ("_pad1", ctypes.c_uint32), ("rt", ctypes.c_void_p)]


def load_pe(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    size = struct.unpack_from("<I", d, pe + 24 + 56)[0]
    img = bytearray(size)
    img[:0x400] = d[:0x400]
    so = pe + 24 + optsz
    for i in range(nsec):
        vs, va, rs, ro = struct.unpack_from("<IIII", d, so + 40 * i + 8)
        n = min(rs, vs or rs, len(d) - ro)
        img[va:va + n] = d[ro:ro + n]
    return base, bytes(img)


def open_lib(path):
    lib = ctypes.CDLL(path)
    lib.test_mem.restype = ctypes.c_void_p
    lib.test_trap_msg.restype = ctypes.c_char_p
    lib.test_run_ms.argtypes = [ctypes.c_uint32, ctypes.POINTER(Cpu), ctypes.c_int]
    assert lib.test_sizeof_cpu() == ctypes.sizeof(Cpu)
    assert lib.test_init() == 0
    return lib


if TRACE_DIFF:
    lib = open_lib(f"{CONF}/libgame_trace.so")
    lib.test_trace.restype = ctypes.POINTER(ctypes.c_uint32)
    lib.test_trace.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
    _blocks = open(f"{CONF}/blocks.bin", "rb").read()
    BLOCKS = set(struct.unpack(f"<{len(_blocks) // 4}I", _blocks))
else:
    lib = open_lib(f"{CONF}/libgame.so")
MEM = lib.test_mem()


def nwrite(addr, data):
    ctypes.memmove(MEM + addr, bytes(data), len(data))


def nread(addr, n):
    return ctypes.string_at(MEM + addr, n)


IMG_BASE, IMG = load_pe(EXE)
IMG_SIZE = (len(IMG) + 0xFFF) & ~0xFFF
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(IMG_BASE, IMG_SIZE)
for base, size in ((LOW, 0x10000), (SCRATCH, SCRATCH_SIZE), (STACK, STACK_SIZE), (SENTINEL, 0x1000)):
    uc.mem_map(base, size)
uc.mem_write(SENTINEL, b"\xf4" * 16)
rng = random.Random(7)


def scratch_content():
    out = bytearray(SCRATCH_SIZE)
    for o in range(0, SCRATCH_SIZE, 4):
        r = rng.random()
        if r < 0.45:
            v = SCRATCH + (rng.randrange(0, SCRATCH_SIZE - 0x1000) & ~3)
        elif r < 0.65:
            v = rng.randrange(0, 16)
        elif r < 0.85:
            v = struct.unpack("<I", struct.pack("<f", rng.uniform(-100, 100)))[0]
        else:
            v = rng.getrandbits(32)
        struct.pack_into("<I", out, o, v)
    return bytes(out)


def arg():
    r = rng.random()
    if r < 0.5:
        return SCRATCH + (rng.randrange(0, SCRATCH_SIZE - 0x1000) & ~3)
    if r < 0.8:
        return rng.randrange(0, 32)
    return rng.getrandbits(32)


def to_f80(v):
    if v == 0:
        return (0, 0x8000 if math.copysign(1, v) < 0 else 0)
    m, e = math.frexp(abs(v))
    mant = int(m * (1 << 64)) & ((1 << 64) - 1)
    return (mant, (e - 1 + 16383) | (0x8000 if v < 0 else 0))


def from_f80(t):
    mant, exp = t
    sign = -1.0 if exp & 0x8000 else 1.0
    e = exp & 0x7FFF
    if e == 0 and mant == 0:
        return 0.0 * sign
    if e == 0x7FFF:
        return float("nan") if (mant << 1) & ((1 << 64) - 1) else sign * float("inf")
    try:
        return sign * math.ldexp(mant, e - 16383 - 63)
    except OverflowError:
        return sign * float("inf")


def floats_close(a: bytes, b: bytes) -> bool:
    if len(a) == 8:
        x, y = struct.unpack("<d", a)[0], struct.unpack("<d", b)[0]
    else:
        x, y = struct.unpack("<f", a)[0], struct.unpack("<f", b)[0]
    if math.isnan(x) and math.isnan(y):
        return True
    return math.isfinite(x) and math.isfinite(y) and abs(x - y) <= 1e-9 * max(abs(x), abs(y), 1e-30)


def compare_region(name, addr, ub, nb, diffs):
    if ub == nb:
        return
    i = 0
    while i < len(ub):
        if ub[i] != nb[i]:
            w = i & ~3
            if floats_close(ub[w:w + 4], nb[w:w + 4]) or floats_close(ub[w & ~7:(w & ~7) + 8], nb[w & ~7:(w & ~7) + 8]):
                i = w + 4
                continue
            diffs.append(f"{name} @{addr + i:08x}: unicorn={ub[w:w+8].hex()} native={nb[w:w+8].hex()}")
            return
        i += 1


def make_state():
    return {
        "regs": [arg() for _ in GPRS],
        "args": [arg() for _ in range(10)],
        "stack": bytes(rng.getrandbits(8) for _ in range(STACK_SIZE)),
        "fregs": [rng.uniform(-100, 100) for _ in range(8)],
    }


def load_state(st, scratch):
    esp = ESP0 - 4 * len(st["args"]) - 4
    frame = struct.pack("<I", SENTINEL) + b"".join(struct.pack("<I", a) for a in st["args"])
    for w, data in ((IMG_BASE, IMG), (SCRATCH, scratch), (STACK, st["stack"]), (LOW, bytes(0x10000))):
        uc.mem_write(w, data)
        nwrite(w, data)
    uc.mem_write(esp, frame)
    nwrite(esp, frame)
    regs = list(st["regs"])
    regs[4] = esp
    for k, r in enumerate(UC_REG):
        uc.reg_write(r, regs[k])
    uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    # Identical x87 state on both sides: TOP=6 (two values on the stack), all 8 registers set.
    uc.reg_write(X.UC_X86_REG_FPCW, 0x27F)
    uc.reg_write(X.UC_X86_REG_FPSW, 6 << 11)
    uc.reg_write(X.UC_X86_REG_FPTAG, 0x0FFF)
    for k in range(8):
        uc.reg_write(getattr(X, f"UC_X86_REG_FP{k}"), to_f80(st["fregs"][k]))
    c = Cpu()
    for k, g in enumerate(GPRS):
        setattr(c, g, regs[k])
    c.eflags, c.df, c.fs_base, c.ftop, c.fcw, c.fsw = 0x202, 0, 0, 6, 0x27F, 0
    for k in range(8):
        c.fpr[k] = st["fregs"][k]
    return c


def run_unicorn(addr, trace=None):
    hook = None
    if trace is not None:
        hook = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: trace.append(a))
        uc.ctl_flush_tb()  # cached translation blocks would otherwise bypass the new hook
    try:
        uc.emu_start(addr, SENTINEL, count=2_000_000)
        return None if uc.reg_read(X.UC_X86_REG_EIP) == SENTINEL else "did not return"
    except UcError as e:
        return str(e)
    finally:
        if hook is not None:
            uc.hook_del(hook)


def compare(c):
    diffs = []
    for k, g in enumerate(GPRS):
        if uc.reg_read(UC_REG[k]) != getattr(c, g):
            diffs.append(f"{g}: unicorn={uc.reg_read(UC_REG[k]):08x} native={getattr(c, g):08x}")
    utop = (uc.reg_read(X.UC_X86_REG_FPSW) >> 11) & 7
    if utop != c.ftop:
        diffs.append(f"x87 TOP: unicorn={utop} native={c.ftop}")
    else:
        for i in range(8):
            phys = (utop + i) & 7
            a, b = from_f80(uc.reg_read(getattr(X, f"UC_X86_REG_FP{phys}"))), c.fpr[phys]
            if not (a == b or (math.isnan(a) and math.isnan(b)) or abs(a - b) <= 1e-9 * max(abs(a), abs(b))):
                diffs.append(f"st{i}: unicorn={a!r} native={b!r}")
                break
    data_lo = 0x5E0000
    compare_region("data", data_lo, bytes(uc.mem_read(data_lo, IMG_BASE + IMG_SIZE - data_lo)), nread(data_lo, IMG_BASE + IMG_SIZE - data_lo), diffs)
    compare_region("scratch", SCRATCH, bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE)), nread(SCRATCH, SCRATCH_SIZE), diffs)
    compare_region("stack", STACK, bytes(uc.mem_read(STACK, STACK_SIZE)), nread(STACK, STACK_SIZE), diffs)
    compare_region("low", LOW, bytes(uc.mem_read(LOW, 0x10000)), nread(LOW, 0x10000), diffs)
    return diffs


def trace_diff(addr, st, scratch):
    """Replay one vector with tracing; report the first block where execution diverges."""
    ut = []
    c = load_state(st, scratch)
    run_unicorn(addr, ut)
    lib.test_trace_reset()
    lib.test_run_ms(addr, ctypes.byref(c), 1000)
    n = ctypes.c_uint32()
    p = lib.test_trace(ctypes.byref(n))
    nt = [p[i] for i in range(n.value)]
    ub = [a for a in ut if a in BLOCKS]
    for i in range(min(len(ub), len(nt))):
        if ub[i] != nt[i]:
            return f"diverges at block #{i}: unicorn->{ub[i]:08x} native->{nt[i]:08x}; previous {[hex(x) for x in nt[max(0, i - 4):i]]}"
    return f"same block sequence ({len(nt)} native, {len(ub)} unicorn): divergence inside a block"


funcs = json.load(open(f"{CONF}/pure.json"))
if ONLY is not None:
    funcs = [f for f in funcs if f["addr"] == ONLY]
rng.shuffle(funcs)
funcs = funcs[:MAXF]
results = {"pass": 0, "fail": 0, "skip": 0}
failing = []
scratch = scratch_content()
for fi, f in enumerate(funcs):
    addr = f["addr"]
    fdiffs = []
    for v in range(VECTORS):
        if v % 3 == 2:
            scratch = scratch_content()
        st = make_state()
        c = load_state(st, scratch)
        if run_unicorn(addr):
            results["skip"] += 1
            continue
        rc = lib.test_run_ms(addr, ctypes.byref(c), 1000)
        diffs = [f"native rc={rc}: {lib.test_trap_msg().decode()}"] if rc else compare(c)
        if diffs:
            if TRACE_DIFF and not fdiffs:
                diffs.append(trace_diff(addr, st, scratch))
            fdiffs.append(diffs)
    if fdiffs:
        results["fail"] += 1
        failing.append((addr, f["insns"], len(fdiffs), fdiffs[0]))
    else:
        results["pass"] += 1
    if (fi + 1) % 100 == 0:
        print(f"... {fi + 1}/{len(funcs)} {results}", file=sys.stderr, flush=True)

print(results)
for addr, n, k, d in sorted(failing, key=lambda x: x[1])[:60]:
    print(f"  {addr:08x} ({n} insns) {k}/{VECTORS} vectors: {d[:3]}")
