# Toolchain Setup — Building CE 3.0 for Dreamcast (Path B)

There are **three** toolchains in play, for three different jobs. Don't conflate them.

| # | Toolchain | Job | Output format |
|---|-----------|-----|----------------|
| 1 | **gweslab/WindowsCE-Build-Tools** (MS SH compiler, NO license) | Compile **kernel + OAL + drivers** | PE/COFF SHx `.exe`/`.dll`/`.lib` |
| 2 | **eMbedded Visual C++ 3.0** (or the DC SDK in `C:\wcedreamcast`) | Build **apps** that run on CE | PE/COFF SHx `.exe`/`.dll` |
| 3 | **KallistiOS / sh-elf-gcc** | Boot stub, image scrambling, hardware **reference** | ELF/raw (NOT for CE) |
| (opt) | **Platform Builder 3.0** | Convenience: `makeimg`/`romimage` + PUBLIC module binaries | — |

> **Platform Builder 3.0 is NO LONGER a hard dependency.** The
> [gweslab/WindowsCE-Build-Tools](https://github.com/gweslab/WindowsCE-Build-Tools) repo ships
> the **SH cross-compiler** (`bin/I386/SH/cl.exe` + `c1/c1xx/c2.dll` + `shasm.exe`), `link`,
> `nmake`, `rc`, `lib`, plus **OAK + DDK headers** (`ce3-oak/INC`: GPE.H, DISPDRVR.H, CXPORT.H,
> ATAPI.H …). That covers everything needed to *compile* the leaked SHX kernel, the OAL, and
> DC drivers — the part the PB-3.0 key was gating. No license, no CD key.
>
> **The two gaps I previously listed are already filled by the DC SDK itself** (see
> `01-findings.md`). `C:\wcedreamcast` ships **`makeimg.exe` + `romimage.exe` + `fmerge.exe` +
> `bingen.exe`**, **all ~90 OS module binaries loose** (incl. `nk.exe`, `gwes`, `filesys`,
> `coredll`, `shell.exe`), and the **full `.bib`/`.reg`/`.dat`** definition. So:
> - **Image packer**: *reuse the SDK's `makeimg`* — no PB, no custom packer, no extraction.
> - **OS module binaries**: *already loose* in `release/{retail,debug}/OS|CoreOS_*` — reuse directly.
> - `nmake` (from gweslab) still replaces `build.exe` when recompiling kernel/driver sources.
>
> Net: **PB 3.0 is not needed at all.** The only thing the SDK lacks for a *desktop* shell is an
> SH-4 `explorer.exe` (build a custom launcher on `gwes`, or port one).

> CE uses Microsoft's **SHx PE/COFF** ABI. `sh-elf-gcc` (KOS) produces ELF and **cannot**
> compile CE — keep it only for the loader/IPL and as a register-level reference.

---

## 0. Host environment
- A **Windows XP SP3 VM** is the path of least resistance for PB 3.0 (the IDE + setup are
  from 2000). The command-line build tools (`build.exe`, `nmake`, `cl shx`, `makeimg`) also
  run from a plain `cmd` on Win10/11, but the installer/IDE often don't — install in the VM,
  then you can copy the tool tree out if you want CLI builds on the host.
- Keep everything on a short path (`C:\WINCE300`) — the build system hates spaces/long paths.

## 1. gweslab build tools (the SH compiler)  — PRIMARY, no license
1. `git clone https://github.com/gweslab/WindowsCE-Build-Tools` (≈470 MB).
2. SH compiler lives at `bin/I386/SH/` (`cl.exe`, `c1.dll`, `c1xx.dll`, `c2.dll`, `shasm.exe`);
   host tools (`link`, `nmake`, `rc`, `lib`, `dumpbin`, `editbin`) in `bin/I386/`.
3. OAL/driver headers in `ce3-oak/INC` (and `ce3-ddk`); app SDK headers/libs per CE version.
4. Build a component by hand: set `PATH` to `bin/I386` and `bin/I386/SH`, set `INCLUDE`/`LIB`
   to the OAK + leaked-source dirs, then `nmake` the SOURCES file (no `build.exe` needed).
   Smoke test: compile one leaked `SHX/*.c` to a `.obj` before wiring up the whole kernel.

## 1b. Platform Builder 3.0 (optional convenience, NOT required)
1. Install **Microsoft Windows CE Platform Builder 3.0** (a.k.a. CE 3.0). During setup pick
   the **SH4** (SH-4) CPU and a sample SH-4 platform (e.g. an SH-4 reference board) — this
   pulls in the **SH-4 C/C++ compiler** (`shcl.exe`/`clshx`), `build.exe`, `nmake`,
   `makeimg.exe`, `romimage.exe`, `cvrtbin.exe`, and the **PUBLIC** tree (binaries for
   coredll, gwes, filesys, the shell, common drivers).
2. It installs to `%_WINCEROOT%` (typically `C:\WINCE300`). Note the env script
   `WINCE300\PUBLIC\COMMON\OAK\MISC\WINCE.BAT` — that's how you enter a build window:
   ```
   WINCE.BAT SH4 SH4 Dreamcast      REM  <CPU> <CPUDIR> <PLATFORM>
   ```
3. Sanity check: `build -c` inside a stock SH-4 platform should produce an `nk.bin`. Get a
   **stock SH-4 image building before touching anything DC-specific.**

## 2. Overlay the leaked kernel source
- The leak's `PRIVATE\WINCEOS\...` mirrors PB's `PRIVATE` layout. To rebuild the kernel with
  your changes, copy (or junction) the leaked tree over `C:\WINCE300\PRIVATE\WINCEOS`.
  Keep a pristine copy; version your edits in **`dc-bsp`** (this repo) and apply as patches.
- The SH-4 kernel you'll edit: `PRIVATE\WINCEOS\COREOS\NK\KERNEL\SHX\` +
  `...\NK\KERNEL\{SCHEDULE,VIRTMEM,PHYSMEM,LOADER}.C`.
- Build just the kernel after the OAL is up; for everything else (gwes/shell/coredll) use
  PB's prebuilt PUBLIC binaries — you are **not** rebuilding all of CE, only kernel + BSP.

## 3. The Dreamcast BSP (what we author — lives in `dc-bsp/`)
Create a platform under `C:\WINCE300\PLATFORM\Dreamcast` by cloning PB's sample SH-4 BSP,
then replacing its OAL + drivers. Mapping to this repo:
```
dc-bsp/oal/dreamcast/   -> PLATFORM\Dreamcast\KERNEL\HAL   (startup.s, OEMInit, TMU tick,
                                                            INTC/Holly ISR, SCIF debug, KITL)
dc-bsp/drivers/         -> PLATFORM\Dreamcast\DRIVERS      (PVR2 display, Maple HID,
                                                            AICA wavedev, GD-ROM block, BBA NDIS)
dc-bsp/files/           -> platform.bib / .reg / .dat / .db (what goes in the image)
dc-bsp/cesysgen/        -> sysgen settings: which modules (shell, explorer, IE) to include
```
Bring-up milestone order (each is a real checkpoint):
**SCIF serial printf → TMU system tick → MMU/cache enable → KITL over serial/BBA →
PVR2 framebuffer display → Maple keyboard/pad → mount GD-ROM → launch the shell.**

## 3b. Rebuild the image with the SDK's own makeimg (the real Path B loop)
Everything below uses tools already in `C:\wcedreamcast`. No PB, no extraction.
```
cd C:\wcedreamcast
wce.bat retail              REM sets WCEDREAMCASTROOT, _PUBLICROOT, _FLATRELEASEDIR, IMG/INIT vars
REM ... edit release\retail\*.bib / *.reg here (add modules, launch entries, your apps) ...
makeimg                     REM fmerge .bib/.reg -> romimage -> RAMIMAGE @ 8C010000 -> nk.bin
REM ... then wrap for the DC loader (Sega 2KB header / scramble) ...
bingen ...                  REM produce the bootable 0winceos.bin-style image + 1ST_READ
```
Milestones:
1. **Round-trip first**: run `makeimg` *unmodified* and diff the result against the shipped
   `0winceos.bin` to prove the pipeline reproduces the image before you change anything.
2. **Add to the image**: drop your `.exe` into `release\retail\OS`, add a line to a `.bib`
   (`myapp.exe  $(_FLATRELEASEDIR)\OS\myapp.exe  NK  SH`) and a `HKLM\init` `LaunchNN`/`DependNN`
   entry in a `.reg`, then `makeimg`. This is how you add a **GUI shell / launcher** and enable
   a multitasking desktop.
3. **Swap the kernel**: recompile `nk.exe` from leaked `SHX` source with the gweslab SH compiler,
   replace `release\retail\nknodbg.exe`, `makeimg` again.

## 4. App toolchain (Path A / writing CE apps) — optional but handy
- Use **eVC++ 3.0** + the DC SDK already in `C:\wcedreamcast` (`wce.bat` sets SH-4 include/lib
  and the VCCE compiler wrapper). Good for building test `.exe`s to throw at the kernel.

## 5. On-target loading & mastering
- **Dev loop (no burning):** `dcload-ip` + Broadband Adapter (RTL8139C) or `dcload-serial`
  + Coder's Cable. Wrap your `nk.bin` in a tiny bootstrap that copies it to `0x0C010000`
  and jumps (mirror Sega's wrapper; see findings doc for the header shape).
- **Bootable GD-R/CDI:** scramble `1ST_READ.BIN` + build `IP.BIN` (`makeip`), master with the
  SDK's **GD Workshop** or `mkisofs`+`cdi4dc`.
- **Emulators for fast iteration:** PB's **CE SH4 emulator** (run the OS image with no DC HW
  for kernel/shell bring-up); **Flycast / lxdream** for the final wrapped image (lxdream has
  the best low-level debug, but peripheral fidelity varies — trust real HW for drivers).

## 6. Hardware reference (don't reinvent register maps)
- **KallistiOS** source = the proven map for SH-4 INTC/TMU, Holly/ASIC IRQ, PVR TA, Maple,
  AICA, GD-ROM, BBA. The OAL/driver work is essentially re-expressing KOS init in CE's
  OAL/stream-driver conventions. Keep a KOS checkout under `loader/` for reference + the IPL.

---
### What you cannot get around
- You need an **SH PE/COFF compiler** (gweslab supplies it free) — `sh-elf-gcc` cannot build CE.
- You need **OS module binaries** for anything you don't compile yourself (gwes/filesys/shell):
  reuse them from `0winceos.bin` or a CE eval. The leak is source for the kernel core only.
- You need a way to **get the image onto/loaded by the DC**: a bootstrap that loads `nk.exe`
  to `0x0C010000`/`0x8C040000` and a filesystem the kernel can load modules from.
