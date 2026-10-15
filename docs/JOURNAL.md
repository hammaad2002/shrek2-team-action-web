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
