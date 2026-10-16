# Journal

Working log for the Shrek 2 Team Action web port (the universal-modder "MODLOG"). Facts, versions,
decisions and gotchas — no decompiled code.

## 2026-10-16 — M0/M1
- Disc: `SHRK2TMA_1.iso`, volume `SHRK2TMA_1`, EN retail. MSI `Shrek 2 Team Action.msi` + `Englis~1.cab`
  (LZX window 2^21, 303 folders, 4,082 files named by MSI File keys) + 23 uncompressed files.
- `Game/Shrek 2.exe`: sha256 `0a750bcf17be4ac106f8e3813d2357b3eb0e6f03c1108eb28da1b37d4bc961f9`,
  md5 `c3bdca1f047e722e72afaa899841ed5c`. Its MsiFileHash (`bccc57f6…`) does not match: the exe is
  dated 2004-10-07 22:09, after the MSI (2004-10-06), and is stored uncompressed — a late swap at
  mastering, not a patch (no crack signatures; the CD routine's result is unused anyway).
- Installer (`installer/`): pure TypeScript ISO9660/Joliet + CFB + MSI tables + CAB + LZX decoder.
  Full install 4,088 files / 862 MB in ~25 s (Node 24); 4,041 MD5-verified via MsiFileHash; all
  4,082 CAB members byte-identical to `cabextract`.
- REA (Ghidra 12.1.4): 6,161 procedures. WinMain = `0x4a8b50`. `0x590fd0(label)` scans drives for a
  CD-ROM with a given volume label; WinMain calls it with "SHREK2" and ignores the result.
- Registry: `HKCU\Software\Activision\Shrek 2 Team Action\Settings` values `resolution` (0..3 →
  1280x1024? / 1024x768 / 800x600 / 640x480 table in WinMain), `mousewheel`, `mousebindL/R/M`,
  `mouseenable`, `mousesens`, `options` (bit flags), `distance`, plus key/pad bindings.

## 2026-10-16 — M2 recompiler (C backend) + conformance
- `recomp/`: iced-x86 decoding, discovery (entry + calls + RTTI vtables + speculative data/imm
  pointers to a fixpoint, Ghidra inventory as hints), MSVC jump tables (`cmp/ja`, `and` mask, byte
  index tables), IR, flag plan (liveness + reaching defs; snapshot vs materialize), x86/x87 lifter,
  C backend. Whole exe: 7,482 functions, ~2M lines of C in ~4 s; compiles cleanly (clang -O1, 18 s).
- Unsupported (trap at runtime): MMX/SSE/3DNow! — only in D3DX CPU-specific paths; CPUID reports a
  Pentium II without MMX/SSE so those paths stay off.
- Conformance (Unicorn 2.1.4 vs native): 809 snippets / 267 instruction forms, 47,680 random runs.
  Bugs found and fixed: register operands not snapshotted before overwrite (flags read new value);
  4 GiB guest window needs a guard tail for unaligned accesses at 0xFFFFFFFx; x87 PC=24 must round
  the significand only (Veltkamp split), not cast to float (exponent range); fsin/fcos/fptan leave
  |x| >= 2^63 unchanged; `rep cmps/scas` with ECX=0 leaves flags unchanged.
- Known Unicorn gaps: FS_BASE writes ignored in 32-bit mode; #DE stops emulation without an error;
  legacy QEMU x87 helpers (fyl2x/fprem/fscale) differ from hardware.
- Resolution registry value: 0 → 640x480 (default), 1 → 800x600, 2 → 1024x768, 3 → 1280x1024.

## 2026-10-16 — M2 function-level conformance, M3 native boot
- Whole-function differential tests (tests/conformance/funcs.py): 1,530 self-contained game
  functions × 6 random states in Unicorn vs native. Fixed: stale regenerated code, x87 trig with
  |x| >= 2^63 (C2 set, operand unchanged, fsincos/fptan don't push). `TRACE_DIFF=1` replays a
  failing vector with block tracing (RT_BLOCK) and reports the first divergent block.
- Known semantic gaps (all rare, documented): x87 sticky exception bits (IE/ZE/PE…) not modelled in
  FSW; rounding-control directed rounding only applied to fist/frndint (D3DX texture code runs whole
  loops with RC=truncate → 1-ulp float differences); fnstenv reserved words.
- Native runtime (runtime/, CMake + SDL2): guest window at fixed host base, dlmalloc mspace heaps,
  VirtualAlloc pages, PE loader with HLE thunks, threads/TEB/TLS, VFS (case-insensitive, game sees
  itself at C:\Games\Shrek 2 Team Action\), virtual registry (DisableD3DXPSGP=1), kernel32 (all 98
  imports), user32/gdi32/winmm/advapi32.
- First run: CRT startup completes, the streaming thread (0x5347d0) starts, the window is created and
  the game's WndProc runs, then it stops at the first DirectX call (DirectSoundCreate8).

## 2026-10-16 — M4/M5 first light: intro videos and the main menu render natively
- D3D9 HLE records a command stream (copies of all guest data) executed by a GLES 3.0 backend
  (`runtime/src/gles/`), the same API/shading language WebGL 2 gives the web build. Render targets
  are stored D3D-style (row 0 = top): clip-space Y is flipped in every vertex shader, so D3D
  viewport/scissor rectangles and texture rows map 1:1 and only the final present blit flips.
  D3D9's half-pixel offset is applied in clip space (+w/vp.w, +w/vp.h after the flip), z goes from
  [0,w] to [-w,w], D3DCULL_CCW → glCullFace(GL_BACK) with GL_CCW front faces.
- Fixed-function pipeline generated as GLSL from a byte key (lights, material sources, fog modes,
  texture coordinate generation/transforms with D3D's (u,v,1,0) padding, all 26 texture ops, alpha
  test). The exe embeds exactly one vs.1.1 shader (planar shadow projection); vs_1_1/vs_2_0 bytecode
  is translated to GLSL by `sm_vs.cpp`. No pixel shaders are used.
- binkw32 HLE (12 exports) parses the BIK container itself and decodes with FFmpeg's libavcodec
  (system FFmpeg 4.4 natively). Video goes through BinkCopyToBuffer into a locked X8R8G8B8 texture;
  music/voice are audio-only Bink files (4x4 video) opened from memory with BINKFROMMEMORY
  (0x04000000) on the streaming thread. Playback timing follows the audio clock.
  Pitfall: a failing `avcodec_receive_frame()` unrefs its output frame — receive into a scratch frame.
- Recompiler: CRT memcpy's jump tables broke discovery twice — LeadUpVec has a garbage entry 0
  (overlapping the previous instruction; index is always 1..3) and TrailUpVec is indexed with a
  negative index. Masked tables now keep only in-.text entries (others trap), and unbounded
  `jmp [r*4+table]` lowers to a switch on the loaded target address over code pointers found around
  the table, with dynamic dispatch as the default.
- Bug: the implicit back buffer was not ref-counted for its render-target binding, so the game's
  first SetRenderTarget deleted it while COM still mapped its id. COM objects now unregister their
  guest wrappers in the destructor (stale pointers fail loudly) and D3D ref-counting is centralised.
- Result: Activision/DreamWorks intro and Beenox logo videos play with audio, the storybook main
  menu renders correctly at 60 fps, and the idle attract-mode demo video starts.
- Test hooks: `SHREK2_HIDDEN=1` (hidden window), `SHREK2_DUMP=n,..` (frame dumps to work/frames),
  `SHREK2_VSYNC=0`, `SHREK2_TRACE=com,hle,draws,uploads,bink,shaders,debug`.

## 2026-10-16 — New game → storybook intro → first in-engine cutscene (native)
- Win32 mutex semantics matter: the streaming thread (0x5347d0) holds the sound mutex across its
  Sleep(1) and re-waits immediately after ReleaseMutex. NT hands a released mutex to the longest
  waiter; a naive condvar mutex lets the releasing thread win every time and the main thread's
  10 ms waits time out → ~3 fps menus. MutexObject now hands ownership over FIFO.
- The game writes `Save\save.bin` (+`_2`, `_3`) with CreateFileA(OPEN_ALWAYS) relative to the game
  directory and never creates the folder; the MSI has no CreateFolder entry for it either. The
  runtime creates `Game/Save` at startup.
- Test tooling: `SHREK2_INPUT="frame:Key[+hold],..."` (scripted key presses by presented frame),
  `SHREK2_WATCHDOG=s` (per-thread HLE activity + guest backtraces when presents stall),
  `SHREK2_STATUS=1` (periodic thread status).
- Result: New Game → create save → save slot A → storybook intro → in-engine cutscene (Shrek and
  Fiona in the swamp, subtitles) at 60 fps, ~630 draws / 81k triangles per frame.

## 2026-10-16 — Performance pass (native)
- Built-in sampling profiler: `SHREK2_PROFILE=start_s:duration_s` (SIGPROF every ms of CPU time →
  work/profile.txt + process map), symbolised by `tools/profile.py` (binary + shared libraries).
- The generated code is compiled at -O1, where clang left the `static inline` guest memory accessors
  and `rt_round24` out of line: >55% of CPU time. They are now `always_inline` (RT_INLINE):
  cutscenes went from 11–22 fps to 150–190 fps uncapped.
- The engine paces itself (0x52df90): a 30 Hz tick model with a speed scale clamped to
  [0.8, 1.35], sleeping the remainder of each frame — so ~30–40 fps in menus/gameplay is the
  intended behaviour (Xbox heritage), and the CPU is mostly idle there.
- Next hotspot: D3DX's x87 D3DXMatrixMultiply (0x56367b) — the x87 register stack lives in the Cpu
  struct with dynamic indexing; a static stack-depth lowering would let it live in locals.

## 2026-10-16 — M7: the web dev build runs the game in Chrome
- Emscripten 6.0.12 (`work/tools/emsdk`), FFmpeg 7.1.1 libavcodec with only the bink /
  binkaudio_rdft / binkaudio_dct decoders (`tools/build-ffmpeg-web.sh`).
- Web runtime: PROXY_TO_PTHREAD (the C main() becomes the game's main thread), guest window
  identity-mapped at [0, 1 GiB) with `-sGLOBAL_BASE=1 GiB`, 2 GiB fixed shared memory, WASMFS with
  the OPFS backend mounted at /opfs. The page's main thread owns the WebGL 2 context and executes
  queued D3D command batches once per requestAnimationFrame (one presented frame per tick; the
  game thread waits at Present while more than one frame is queued). Audio: a feeder pthread
  renders the mixer into a shared ring read by an AudioWorklet. Input: DOM events → exported
  web_* functions (KeyboardEvent.code → VK + DirectInput scan code table).
- Shell (web/, Vite 8): OPFS installer worker reusing installer/src (FileReaderSync over the
  chosen File, FileSystemSyncAccessHandle writes), Play button, dev switches (devIso, autoplay,
  keys, capture). The dev engine (contains translated code) is only served by a dev-server
  middleware from work/build-web, so `vite build` output can never include it.
- Headless smoke test (`tools/web-smoke.mjs`, system Chrome + playwright-core, persistent
  profile in work/): install from the ISO into OPFS 4,088 files / 822 MB / MD5-verified in ~100 s;
  WebGL 2 on the real GPU (ANGLE/Vulkan); intros at ~58 fps, menus at the game's ~37 fps; New Game
  → save → cutscene → first level with HUD all render correctly.
- Open issue: after creating the save the browser build drops to ~2 fps for a while (native does
  not) — investigating.

## 2026-10-16 — M8: the game is translated inside the player's browser
- WebAssembly backend (`recomp/src/emit-wasm.ts`, `wasm.ts`): same IR as the C backend, one wasm
  function per guest function plus `dispatch(c, addr)` (u16 index over .text → call_indirect);
  shares the runtime's memory; runtime helpers imported as function-table references. Structured
  control flow with Ramsey's dominator-tree algorithm ("Beyond Relooper"); targets of multiway
  branches are treated as merge nodes so br_table/br_if have labels; the 5 irreducible functions
  use a loop + br_table block dispatcher. Division helpers keep recomp.h's no-trap semantics.
- Conformance: `BACKEND=wasm tests/conformance/funcs.py` runs the module under wasmtime with
  cpu_helpers.c compiled against the wasm linear memory (`GUEST_BASE` bound at run time):
  1,521 / 1,530 functions pass — identical to the C backend and with the same 9 known x87 gaps.
  (wasmtime-py 49 double-wraps shared-memory externs; the harness patches `_as_extern`.)
- `metadata/<sha256>.json` (121 KB): address-only discovery result for the supported exe (7,482
  function entries, 46 no-return functions, 790 jump tables). With it the translator skips
  speculative discovery. `recomp/src/translate.ts` is shared by the CLI and the browser worker;
  the module writer streams (lift → encode per function) so the browser never holds the whole IR.
- Browser: `web/src/translate-worker.ts` reads Shrek 2.exe from OPFS, checks its SHA-256 against
  the shipped metadata, translates (Chrome: 7,482 functions → 18.9 MB in 3.9 s) and caches the
  module in OPFS (`shrek2/cache/game-<sha16>-t<version>.wasm`). iced-x86's Node-only build is
  exposed to the browser through a Vite virtual ES module (`initIced(bytes)`; exports aliased to
  avoid clashing with the CommonJS top-level classes).
- Runtime `shrek2_player` (RT_PLAYER): 1.08 MB wasm, no game code; each worker that runs guest
  code instantiates the translated module lazily (synchronous compile from shared memory).
- Result: in headless Chrome the deployable player path installs from the ISO, translates the exe,
  and runs intros, menus and attract demos at the game's normal ~37 fps.

## 2026-10-16 — Focus-loss freeze (reported by the user)
- Symptom: clicking outside the browser froze the game permanently. The WndProc (0x4a7510) clears
  the active flag on WM_ACTIVATE(WA_INACTIVE) and, because the game runs fullscreen
  (0x5e2365), also sets device-lost flags 0x5f4e59/0x5f4e5a. Its recovery routine (0x52df00)
  only calls Reset() when TestCooperativeLevel returns D3DERR_DEVICENOTRESET, which the HLE never
  did — so the loop called the recovery routine forever and never rendered again.
- Fix: D3D9 device-loss emulation (lost on deactivation of a fullscreen device; DEVICELOST while
  inactive, DEVICENOTRESET when active; Present returns DEVICELOST) and Reset() now unbinds
  resources and resets the executor's state (OpResetState). Verified with simulated blur/focus in
  headless Chrome during the intro and during gameplay.

## 2026-10-16 — Save backup, fullscreen and mouse capture
- Saves (`web/src/saves.ts`, `zip.ts`, `opfs-write.ts`): the play panel backs up Game/Save
  (save.bin, save.bin_2, save.bin_3; 512 B each) as one stored ZIP, and restores from a backup
  ZIP, any ZIP holding those names in folders (stored or deflated, CRC-checked), or the loose
  files of a Windows install. Names and sizes are validated first; replacing existing slots needs
  an inline confirmation; one operation at a time. Writes use `createWritable` or, where the main
  thread lacks it, a worker with a sync access handle.
- Game view: a HUD (fullscreen button + hints) appears on mouse movement. Fullscreen uses the
  Fullscreen API on `#game`; Chromium also takes `navigator.keyboard.lock(["Escape"])` so Esc
  reaches the game (hold Esc to leave). Elsewhere the pause-key alias (default Backquote, a
  browser preference in localStorage) is sent as Esc. Clicking the game locks the pointer for the
  mouse camera; that click is not passed to the game.
- The game calls ShowCursor(FALSE) once, when it creates its fullscreen device (0x52d560); the
  web platform now mirrors ShowCursor onto the canvas's CSS cursor.
- New trace channel `input` (window-message pump). `tools/web-ui-test.mjs` checks all of the
  above in headless Chrome (backup ZIP verified independently with Python's zipfile; the game
  opens the restored saves).
