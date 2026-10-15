#!/usr/bin/env python3
"""Symbolise work/profile.txt (raw host PCs from SHREK2_PROFILE) against the native binary and the
shared libraries listed in work/profile_maps.txt."""
import bisect
import collections
import os
import subprocess
import sys

binary = sys.argv[1] if len(sys.argv) > 1 else "work/build-native/shrek2"
samples = sys.argv[2] if len(sys.argv) > 2 else "work/profile.txt"
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
maps_path = os.path.join(os.path.dirname(samples), "profile_maps.txt")


def symbols(path, dynamic=False):
    args = ["nm", "-n", "-C"] + (["-D"] if dynamic else []) + [path]
    out = []
    for line in subprocess.run(args, capture_output=True, text=True).stdout.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[0] and parts[1] in ("t", "T", "w", "W", "i"):
            out.append((int(parts[0], 16), parts[2]))
    return out


# Executable mappings: (start, end, file offset, path).
maps = []
if os.path.exists(maps_path):
    for line in open(maps_path):
        f = line.split()
        if len(f) >= 6 and "x" in f[1]:
            a, b = (int(x, 16) for x in f[0].split("-"))
            maps.append((a, b, int(f[2], 16), f[5]))
main_syms = symbols(binary)
main_addrs = [a for a, _ in main_syms]
lib_syms = {}
counts = collections.Counter()
total = 0
for line in open(samples):
    pc = int(line, 16)
    total += 1
    m = next((m for m in maps if m[0] <= pc < m[1] and not m[3].endswith("/shrek2")), None)
    if m:
        path = m[3]
        if path not in lib_syms:
            syms = symbols(path, dynamic=True) + symbols(path)
            syms.sort()
            lib_syms[path] = (syms, [a for a, _ in syms])
        syms, addrs = lib_syms[path]
        # Shared objects: symbol values are offsets from the load base (first mapping of the file).
        base = min(x[0] - x[2] for x in maps if x[3] == path)
        off = pc - base
        i = bisect.bisect_right(addrs, off) - 1
        name = f"[{os.path.basename(path)}] " + (syms[i][1] if i >= 0 else "?")
    else:
        i = bisect.bisect_right(main_addrs, pc) - 1
        name = main_syms[i][1] if i >= 0 else "?"
    counts[name] += 1
print(f"{total} samples")
for name, n in counts.most_common(top):
    print(f"{100.0 * n / total:6.2f}%  {n:7d}  {name[:120]}")
