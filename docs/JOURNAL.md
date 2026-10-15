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
