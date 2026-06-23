# Dreamcast WinCE — Findings (Path B)

## Stock kernel image (`C:\wcedreamcast\release\retail\0winceos.bin`)
- **DUMPNK rejects it**: "Invalid NK image header." It is NOT a standard CE `.bin`
  (no `B000FF` signature). It's a **Sega-wrapped raw image**:
  - 0x000: magic `D6 1A`, then a **0x800 (2 KB) header**.
  - 0x14 / 0x20: load base **`0x0C010000`** = DC *physical* RAM (0x0C000000) + 64 KB.
  - 0x1C: payload length `0x1CC800` (1,886,208 B); 0x800 + 0x1CC800 = file size.
  - `ECEC` (CE ROMHDR signature) appears at offset 2112 (just past the header) —
    so it *is* a CE ROM, just wrapped by Sega's loader. DUMPNK only knows plain `.bin`.
- **Modules baked into the stock 2.12 image** (extracted from the binary):
  - Full CE core: `COREDLL`, `filesys.exe`, `gwes.exe`, `device.exe`, `ole32`,
    `winsock`, `toolhelp`, `schannel` (SSL), `sysstart.exe`, **`Qshell.exe`** (a minimal shell).
  - DC drivers: `MAPLE`/`MAPLEDEV` (controllers), `SH4SER` (SCIF serial), `TIMER`,
    `DDRAW`/`DDHAL`/`DDI`/`D3DIM` (PowerVR2), `DSOUND`/`SNDCORE` (AICA),
    `MPPP`/`microstk.exe` (PPP + micro TCP/IP), `WATAPI` (GD-ROM ATAPI),
    `wsegacd`/`seg_rock` (GD), `PLATUTIL`.
- **Implication**: GWES (windowing) and a shell stub are already present in 2.12. What's
  missing for "desktop CE" is `explorer.exe`/the HPC shell + a fuller Win32/API set —
  exactly what CE 3.0 + the standard shell components add.

## Leaked source (`C:\dev\Dreamcast\wince-src`, branch WINCE300)
- It is **WINCEOS only**: `NK` (kernel, incl. **SHX/ SuperH with real SH-4 MMU/INTC/TMU**),
  `CORE`, `DEVICE`, `FSDMGR`, `GWE/MGDI`, `COMM` (PPP2/TAPI/DHCP). ~143 source files, 5.4 MB.
- **Not present**: `PUBLIC` tree, full GWES, the shell/explorer, most drivers,
  headers/libs, and **no build tools / no compiler**.
- `SHX/SOURCES` links at `EXEBASE=0x8C040000` (SH-4 *cached* RAM base — DC-shaped).

## ★ The DC SDK is a COMPLETE makeimg build environment (corrected 2026-06-23)
Earlier I only looked at the top of `release/retail` and wrongly concluded image-assembly was a
gap. It is not. `C:\wcedreamcast` ships the **entire** CE image pipeline:
- **Loose kernel**: `release/{retail,debug}/nk.exe` (158 KB) + `nknodbg.exe` (no-debug, what the
  .bib actually uses) + `nkscifkd.exe` (SCIF KITL debug kernel).
- **All ~90 OS module binaries** loose in `release/{retail,debug}/OS/` and `CoreOS_*/`:
  `coredll`, `gwes.exe`, `filesys.exe`, `ole32`, **`shell.exe`** (the CE *command* shell, 73 KB),
  IE stack (`mshtml`,`jscript`,`shdocvw`,`shlwapi`,`wininet`), `winsock`, `schannel`, every DC
  driver (`maple`,`ddraw`,`d3dim`,`dsound`,`sndcore`,`sh4ser`,`mppp`,`microstk`,`wsegacd` …).
- **Full image toolchain** in `tools/`: **`makeimg.exe`, `romimage.exe`, `fmerge.exe`,
  `bingen.exe`**, `regcomp.exe`, `res2exe.exe`, `rebaseic.exe`, `cenlscmp.exe`, `signfile.exe`.
- **Full image definition**: `ce.bib`, `common.bib`, `config.bib`, `platform.bib`,
  **`gemini.bib`** (113 modules), `*.reg`, `*.dat`, `country.cfg`, `initobj.dat`.
- **`makeimg.out`** is the real build log → makeimg was used to produce `0winceos.bin`.

Image layout (from `ce.bib` MEMORY):
```
NK       8C010000  005F0000  RAMIMAGE   ; kernel+modules, cached mirror of 0C010000
RAM      8C600000  009D0000  RAM
DRIVGLOB 8CFD0000  00030000  RESERVED   ; Maple/ATAPI/D3DHAL/GDI shared globals
CONFIG: AUTOSIZE=ON COMPRESSION=ON KERNELFIXUPS=ON ROMFLAGS=1
```
`8C010000` (cached) == the `0C010000` (physical) load base in the Sega wrapper header — consistent.

Boot launch order (`HKLM\init`, from `.reg`): `gwes.exe`(30) → `shell.exe`(11, debug cmd shell) →
`wdevice.exe`(40) → `microstk.exe`(50) → **`sysstart.exe`(60)** = the app that launches the title.
**No `explorer.exe` anywhere in the SDK** — confirms "no desktop shell". `gwes` (windowing) *is*
present, so a GUI shell app has a working surface to draw on.

## Conclusion that drives the toolchain
Revised, much simpler picture:
**Path B = the DC SDK's own makeimg env (modules + makeimg/romimage + .bib/.reg)
  ⊕  the gweslab SH compiler (only to recompile the kernel / build new modules & apps)
  ⊕  the leaked WINCEOS source (only when you want to modify the kernel).**

Platform Builder 3.0 is **not needed at all** — `makeimg`/`romimage`/`fmerge`/`bingen`, every
module binary, and the full `.bib`/`.reg` are already in `C:\wcedreamcast`. You do **not**
extract from the packed image and you do **not** write a ROM packer.

What's genuinely still missing for a *desktop-class* shell: an **`explorer.exe`/GUI shell built
for SH-4** (absent here). Get it by (a) writing a minimal custom launcher app on top of the
present `gwes` — easiest, total control; (b) porting/recompiling a shell from the leaked CE
source; or (c) lifting `explorer.exe` from another SH-4 CE 3.x image (version-compat risk vs 2.12).
Multitasking already exists in the kernel — the runtime simply only launches `sysstart`→title;
a launcher that `CreateProcess`-es several apps *is* the "multitasking shell".
