# Shrek 2 Team Action — web port (bring your own disc)

Play your own copy of *Shrek 2 Team Action* (PC, 2004) in a desktop browser.

This repository contains **no game files and no game code**. Players supply their own disc image
(`SHRK2TMA_1.iso`); the browser unpacks it locally and translates the game's x86 executable to
WebAssembly on the player's machine. The translated code runs on a re-implementation of the
Windows, Direct3D 9, DirectSound, DirectInput and Bink interfaces the game uses.

Status: early development. See `docs/JOURNAL.md`.

## Layout
- `installer/` — ISO 9660 + MSI + CAB/LZX extraction (TypeScript; runs in Node and in a browser worker)
- `recomp/` — x86 → C / WebAssembly static recompiler (TypeScript)
- `runtime/` — Win32/D3D9/DirectSound/DirectInput/Bink re-implementation (C++; native SDL2 + Emscripten)
- `web/` — browser shell
- `metadata/` — per-executable analysis hints (addresses only)

## Developer quick start
```sh
npm install
node installer/src/cli.ts /path/to/SHRK2TMA_1.iso work/install   # extract + verify against the MSI hashes
```
`work/` and `research/` are git-ignored: never commit game files, extracted assets or decompiled code.
