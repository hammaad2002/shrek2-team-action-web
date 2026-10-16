# Shrek 2 Team Action — in your browser (bring your own disc)

Play your own copy of *Shrek 2 Team Action* (PC, 2004) in a desktop browser.

This repository and the site built from it contain **no game files and no game code**. Each
player chooses their own disc image (`SHRK2TMA_1.iso`); the browser unpacks it into its private
storage and translates the game's x86 program to WebAssembly on the player's own machine. The
translated code runs on a re-implementation of the Windows, Direct3D 9, DirectSound, DirectInput
and Bink interfaces the game uses.

Not affiliated with or endorsed by Activision, DreamWorks Animation or Beenox.

## Playing
- **Browsers:** current desktop Chrome, Edge, Firefox or Safari, in a normal (not private)
  window. About 1 GB of storage is needed. Unpacking takes about a minute (Firefox: 3–4 minutes),
  once; translating the game takes a few seconds the first time.
- **Controls:** the game's own keys (arrows to move, Space jump, Z use, X attack, C special, A/S
  switch character, D HUD, Esc pause, numpad camera). Press F1 in game, or **Controls** on the
  start page, for the full list.
- **Controllers:** up to four Xbox-style controllers, one per player, in the order they are
  connected (press a button so the browser reports it). Player 2–4 join with Start.
- **Settings** (the original Launcher's options): resolution, shadows, distance clipping, mouse
  camera, and keyboard and controller bindings for every player.
- **Saves** stay in the browser that made them. **Back up saves** downloads them as a ZIP;
  **Restore saves** reads that ZIP or the `save.bin` files of a Windows installation.
- **Fullscreen** from the button at the top right of the game. Browsers keep Esc for leaving
  fullscreen (Chrome and Edge let the game have it); the substitute key (` by default) works as
  Esc everywhere.

## How it works
1. `installer/` reads the ISO 9660 image, the MSI database and the LZX cabinet and writes the
   installed tree into the Origin Private File System, checking every file against the MSI's
   MD5 hashes.
2. `recomp/` translates `Shrek 2.exe` (identified by SHA-256; `metadata/` holds address-only
   analysis hints for the supported executable) into one WebAssembly module, cached in OPFS.
3. `runtime/` (C++, Emscripten) provides the Windows API, Direct3D 9 on WebGL 2 / OpenGL ES 3,
   DirectSound, DirectInput (keyboard, mouse, four virtual gamepads) and Bink (FFmpeg's
   decoders). The game runs on worker threads; the page renders and handles input.
4. `web/` is the page: install, settings, saves, controls, fullscreen.

`docs/JOURNAL.md` records how each part of the game's behaviour was worked out.

## Building
Requirements: Node.js 24, Emscripten 6.0.12 at `work/tools/emsdk`, the FFmpeg 7.1.1 source at
`work/tools/ffmpeg-dl/ffmpeg-7.1.1`, and for the native build SDL2, FFmpeg and OpenGL ES headers.

```sh
npm install
tools/build-web.sh                       # engine (and FFmpeg for the web on first run) -> web/public/engine
cd web && npx vite                       # development server on http://localhost:5173
REPO_URL=<repository URL> tools/build-release.sh   # deployable site in web/dist, with release checks
```

Development tools (they need a disc image and run against the dev server):
`tools/web-smoke.mjs` (a session with scripted keys and frame captures), `tools/web-ui-test.mjs`
(saves, fullscreen, mouse capture, controls list), `tools/web-pad-test.mjs` (settings and
controller play) — each takes `--browser chrome|firefox|webkit`. The native build
(`runtime/`, SDL2) runs the installed game with scripted input (`SHREK2_INPUT`, `SHREK2_PAD`).

`work/` and `research/` are git-ignored: game files, extracted data and anything derived from
them never enter the repository or the site. `tools/scan-dist.mjs` checks a build for any byte
sequence shared with the game's files before release.

## Licenses
The code in this repository is under the MIT license (`LICENSE`). The engine statically links
FFmpeg 7.1.1 (LGPL 2.1 or later; minimal build by `tools/build-ffmpeg-web.sh`, source at
https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz). Also used: iced-x86 (MIT), Emscripten and its
libraries (MIT; LLVM libc++/compiler-rt under Apache 2.0 with LLVM exceptions), dlmalloc
(MIT-0), Khronos headers (MIT), Vite (MIT). The site's `licenses.html` lists them.
