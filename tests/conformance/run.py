"""
Differential conformance test: run each snippet in Unicorn (reference CPU) and in the
recompiled native library with identical inputs, and compare registers, defined flags,
x87 results and touched memory.

  .venv/bin/python -I tests/conformance/run.py <game.exe> work/conf [vectors-per-snippet] [filter]
"""
import ctypes
import json
import math
import random
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn import x86_const as X

EXE, CONF = sys.argv[1], sys.argv[2]
VECTORS = int(sys.argv[3]) if len(sys.argv) > 3 else 48
FILTER = sys.argv[4] if len(sys.argv) > 4 else None

IO = 0x2FF00000
IO_IN_FLAGS, IO_OUT_FLAGS, IO_IN_FCW, IO_OUT_SW, IO_IN_F, IO_OUT_F = IO, IO + 4, IO + 8, IO + 0xC, IO + 0x10, IO + 0x40
FS_BASE = 0x2FF10000
SENTINEL = 0x2FF20000
SCRATCH, SCRATCH_SIZE = 0x20000000, 0x100000
STACK, STACK_SIZE = 0x30000000, 0x10000
ESP0 = STACK + 0x8000
WINDOW = 0x400

GPRS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UC_REG = {"eax": X.UC_X86_REG_EAX, "ecx": X.UC_X86_REG_ECX, "edx": X.UC_X86_REG_EDX, "ebx": X.UC_X86_REG_EBX,
          "esp": X.UC_X86_REG_ESP, "ebp": X.UC_X86_REG_EBP, "esi": X.UC_X86_REG_ESI, "edi": X.UC_X86_REG_EDI}
REG32 = {"AL": "eax", "AH": "eax", "AX": "eax", "EAX": "eax", "CL": "ecx", "CH": "ecx", "CX": "ecx", "ECX": "ecx",
         "DL": "edx", "DH": "edx", "DX": "edx", "EDX": "edx", "BL": "ebx", "BH": "ebx", "BX": "ebx", "EBX": "ebx",
         "SP": "esp", "ESP": "esp", "BP": "ebp", "EBP": "ebp", "SI": "esi", "ESI": "esi", "DI": "edi", "EDI": "edi"}


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


lib = ctypes.CDLL(f"{CONF}/libconf.so")
lib.test_mem.restype = ctypes.c_void_p
lib.test_trap_msg.restype = ctypes.c_char_p
lib.test_run.argtypes = [ctypes.c_uint32, ctypes.POINTER(Cpu)]
assert lib.test_sizeof_cpu() == ctypes.sizeof(Cpu), "Cpu layout mismatch between C and Python"
assert lib.test_init() == 0, "could not map guest memory"
MEM = lib.test_mem()


def nwrite(addr, data):
    ctypes.memmove(MEM + addr, bytes(data), len(data))


def nread(addr, n):
    return ctypes.string_at(MEM + addr, n)


uc = Uc(UC_ARCH_X86, UC_MODE_32)
regions = []
for base, img in (load_pe(EXE), load_pe(f"{CONF}/snippets.exe")):
    size = (len(img) + 0xFFF) & ~0xFFF
    uc.mem_map(base, size)
    uc.mem_write(base, img)
    nwrite(base, img)
for base, size in ((SCRATCH, SCRATCH_SIZE), (STACK, STACK_SIZE), (IO, 0x1000), (FS_BASE, 0x1000), (SENTINEL, 0x1000)):
    uc.mem_map(base, size)
    regions.append((base, size))
uc.mem_write(SENTINEL, b"\xf4" * 16)
rng = random.Random(1234)
scratch = bytes(rng.getrandbits(8) for _ in range(SCRATCH_SIZE))
uc.mem_write(SCRATCH, scratch)
nwrite(SCRATCH, scratch)
fsdata = bytes(rng.getrandbits(8) for _ in range(0x1000))
uc.mem_write(FS_BASE, fsdata)
nwrite(FS_BASE, fsdata)

manifest = json.load(open(f"{CONF}/manifest.json"))

INTERESTING = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF]


def rand_u32():
    r = rng.random()
    if r < 0.3:
        return rng.choice(INTERESTING)
    if r < 0.5:
        return rng.randrange(0, 64)
    if r < 0.6:
        return (-rng.randrange(1, 64)) & 0xFFFFFFFF
    return rng.getrandbits(32)


def rand_double():
    r = rng.random()
    if r < 0.2:
        return float(rng.randrange(-100, 100))
    if r < 0.3:
        return rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 1e10, -1e-10, 3.141592653589793])
    if r < 0.9:
        return rng.uniform(-1000, 1000)
    return rng.uniform(-1e30, 1e30)


def windows_for(m, regs):
    """Address windows (start, size) touched by the instruction, given register values."""
    wins = [(STACK + 0x7000, 0x2000), (IO, 0x100)]
    if m is not None:
        ea = 0
        if m["base"] != "None":
            ea += regs[REG32[m["base"]]]
        if m["index"] != "None":
            ea += regs[REG32[m["index"]]] * m["scale"]
        ea = (ea + m["disp"]) & 0xFFFFFFFF
        if m["seg"] == "FS":
            ea = (ea + FS_BASE) & 0xFFFFFFFF
        wins.append(((ea - WINDOW // 2) & 0xFFFFFFFF, WINDOW))
    return wins


def setup_regs(s):
    regs = {g: rand_u32() for g in GPRS}
    regs["esp"] = ESP0
    m = s["mem"]
    if m is not None and (m["base"] != "None" or m["index"] != "None"):
        if m["index"] != "None":
            regs[REG32[m["index"]]] = rng.randrange(0, 64)
        if m["base"] != "None" and m["base"] not in ("ESP", "SP"):
            idx = regs[REG32[m["index"]]] * m["scale"] if m["index"] != "None" else 0
            target = SCRATCH + 0x80000 + rng.randrange(-0x4000, 0x4000)
            regs[REG32[m["base"]]] = (target - idx - m["disp"]) & 0xFFFFFFFF
        elif m["base"] == "None":
            idx_reg = REG32[m["index"]]
            target = SCRATCH + 0x80000 + rng.randrange(-0x4000, 0x4000)
            if m["scale"] == 1:
                regs[idx_reg] = (target - m["disp"]) & 0xFFFFFFFF
    if s["string"]:
        regs["esi"] = SCRATCH + 0x40000 + rng.randrange(0, 0x100)
        regs["edi"] = SCRATCH + 0xC0000 + rng.randrange(0, 0x100)
        regs["ecx"] = rng.randrange(0, 40)
    if m is not None and m["base"] in ("ESP", "SP"):
        regs["esp"] = ESP0
    return regs


def run_one(s, regs, flags_in, fcw, fin):
    # Pre-state (identical on both sides).
    io = bytearray(0x100)
    struct.pack_into("<I", io, 0, flags_in)
    struct.pack_into("<I", io, 8, fcw)
    for k, v in enumerate(fin):
        struct.pack_into("<d", io, 0x10 + 8 * k, v)
    stack = bytes(rng.getrandbits(8) for _ in range(0x2000))
    wins = windows_for(s["mem"], regs)
    saved = []
    for (a, n) in wins[2:]:
        if not (SCRATCH <= a and a + n <= SCRATCH + SCRATCH_SIZE) and not (0x400000 <= a < 0x675000) and not (FS_BASE <= a < FS_BASE + 0x1000):
            return "skip"
        saved.append((a, n, nread(a, n)))
    for side_write in (uc.mem_write, nwrite):
        side_write(STACK + 0x7000, stack)
        side_write(IO, bytes(io))
        side_write(ESP0 - 4, struct.pack("<I", SENTINEL))
    # Unicorn
    for g in GPRS:
        uc.reg_write(UC_REG[g], regs[g] if g != "esp" else ESP0 - 4)
    uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    uc_fault = None
    try:
        uc.emu_start(s["addr"], SENTINEL, count=20000)
        if uc.reg_read(X.UC_X86_REG_EIP) != SENTINEL:
            uc_fault = f"stopped at {uc.reg_read(X.UC_X86_REG_EIP):08x}"  # e.g. #DE without an error code
    except UcError as e:
        uc_fault = str(e)
    ur = {g: uc.reg_read(UC_REG[g]) for g in GPRS}
    # Native
    c = Cpu()
    for g in GPRS:
        setattr(c, g, regs[g] if g != "esp" else ESP0 - 4)
    c.eflags, c.df, c.fs_base, c.ftop, c.fcw, c.fsw = 0x202, 0, FS_BASE, 0, 0x37F, 0
    rc = lib.test_run(s["addr"], ctypes.byref(c))
    nat_fault = lib.test_trap_msg().decode() if rc else None
    nr = {g: getattr(c, g) for g in GPRS}
    diffs = []
    if (uc_fault is None) != (nat_fault is None):
        diffs.append(f"fault mismatch: unicorn={uc_fault} native={nat_fault}")
    elif uc_fault is None:
        for g in GPRS:
            if ur[g] != nr[g]:
                diffs.append(f"{g}: unicorn={ur[g]:08x} native={nr[g]:08x}")
        uio = uc.mem_read(IO, 0x100)
        nio = nread(IO, 0x100)
        if not s["x87"]:
            defined = s["flagsModified"] & ~s["flagsUndefined"]
            mask = 0
            for ib, eb in ((1, 11), (2, 7), (4, 6), (8, 4), (16, 0), (32, 2)):
                if defined & ib:
                    mask |= 1 << eb
            mask |= 1 << 10  # DF
            uf, nf = struct.unpack_from("<I", uio, 4)[0], struct.unpack_from("<I", nio, 4)[0]
            if (uf ^ nf) & mask:
                diffs.append(f"flags: unicorn={uf & mask:04x} native={nf & mask:04x} (mask {mask:04x})")
        else:
            depth = 5 - s["fpuInc"]
            for k in range(depth):
                a = struct.unpack_from("<d", uio, 0x40 + 8 * k)[0]
                b = struct.unpack_from("<d", nio, 0x40 + 8 * k)[0]
                same = a == b or (math.isnan(a) and math.isnan(b)) or (abs(a - b) <= 1e-12 * max(abs(a), abs(b)) and s["mnemonic"] in ("Fsin", "Fcos", "Fsincos", "Fpatan", "Fyl2x", "F2xm1", "Fptan"))
                if not same:
                    diffs.append(f"st{k}: unicorn={a!r} native={b!r}")
            usw, nsw = struct.unpack_from("<H", uio, 0xC)[0], struct.unpack_from("<H", nio, 0xC)[0]
            cmask = 0
            defined = s["flagsModified"] & ~s["flagsUndefined"]
            for ib, sb in ((1024, 8), (4096, 10), (8192, 14)):  # C0, C2, C3 (C1 excluded)
                if defined & ib:
                    cmask |= 1 << sb
            if (usw ^ nsw) & cmask:
                diffs.append(f"fsw C-bits: unicorn={usw & cmask:04x} native={nsw & cmask:04x}")
        for (a, n, _) in saved:
            ub, nb = bytes(uc.mem_read(a, n)), nread(a, n)
            if ub != nb:
                k = next(i for i in range(n) if ub[i] != nb[i])
                diffs.append(f"memory @{a + k:08x}: unicorn={ub[k:k+8].hex()} native={nb[k:k+8].hex()}")
        us, ns = bytearray(uc.mem_read(STACK + 0x7000, 0x2000)), bytearray(nread(STACK + 0x7000, 0x2000))
        slot = ESP0 - 8 - (STACK + 0x7000)  # pushfd slot holds architecturally undefined flag bits
        us[slot:slot + 4] = ns[slot:slot + 4] = b"\0\0\0\0"
        if us != ns:
            k = next(i for i in range(len(us)) if us[i] != ns[i])
            diffs.append(f"stack @{STACK + 0x7000 + k:08x} differs")
    # Restore windows.
    for (a, n, data) in saved:
        uc.mem_write(a, data)
        nwrite(a, data)
    return diffs


fails = {}
runs = skipped = 0
for s in manifest:
    if FILTER and FILTER.lower() not in s["text"].lower():
        continue
    if s["mem"] is not None and s["mem"]["seg"] == "FS":
        continue  # Unicorn's 32-bit mode ignores FS_BASE writes; fs: is covered by function tests
    if __import__("os").environ.get("CONF_TRACE"):
        print("RUN", hex(s["addr"]), s["text"], file=sys.stderr, flush=True)
    for v in range(VECTORS):
        regs = setup_regs(s)
        flags_in = 0x202 | sum(rng.choice([0, 1]) << b for b in (0, 2, 4, 6, 7, 11))
        if s["string"] and rng.random() < 0.2:
            flags_in |= 1 << 10
        fcw = 0x027F if rng.random() < 0.85 else 0x007F
        if s["mnemonic"] in ("Fistp", "Fist", "Frndint") and rng.random() < 0.5:
            fcw |= rng.choice([0x400, 0x800, 0xC00])
        fin = [rand_double() for _ in range(5)]
        if s["mnemonic"] in ("Fyl2x", "Fsqrt"):
            fin[0] = abs(fin[0]) + 1e-3  # QEMU's legacy x87 helpers don't model invalid operands
        if s["mnemonic"] in ("Fprem", "Fprem1", "Fscale") and fcw & 0x300 == 0:
            fcw |= 0x200  # QEMU applies precision control to ops that hardware doesn't round
        res = run_one(s, regs, flags_in, fcw, fin)
        if res == "skip":
            skipped += 1
            continue
        runs += 1
        if res:
            fails.setdefault(s["text"], []).append((regs, flags_in, fcw, fin, res))

print(f"{runs} runs, {skipped} skipped, {len(fails)} failing snippets")
for text, fl in sorted(fails.items(), key=lambda kv: -len(kv[1]))[:60]:
    regs, flags_in, fcw, fin, res = fl[0]
    print(f"  {len(fl):3d}x {text:40s} {res[0]}")
