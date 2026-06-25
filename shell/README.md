# dcshell — a desktop shell/launcher for Dreamcast Windows CE 2.12

The piece the SDK doesn't have: there's **no `explorer.exe`** anywhere (SDK, leak, or any
PB), and GWES/shell source was never released — so the shell is the one component you **write**,
not acquire (see `../docs/06`/`../docs/08` and the NT4/PB analyses). It's the *easy* part: the
stock closed 2.12 already has **multitasking** (the kernel is preemptive — it boots
gwes+shell+wdevice+microstk+sysstart concurrently) and a **full GUI/GDI API** (gwes.exe +
`coredll` exports `CreateWindowExW`/`GetMessageW`/`BitBlt`/`ExtTextOutW`/`CreateProcessW`/
`FindFirstFileW`…). `dcshell` is just an ordinary CE GUI app on top.

## What it is (v1)
Full-screen launcher: lists `*.exe` in `\Windows` and `\`, Up/Down (or tap) to select,
Enter/tap to `CreateProcess` it (= multitasking), with a title + live clock.

## The DC GDI subset (important)
The stock `coredll` GDI is **text+blit only** — present: `ExtTextOutW`, `CreateFontIndirectW`,
`BitBlt`, `GetStockObject`, `SelectObject`, `SetBkColor`/`SetTextColor`, the `RECT` helpers,
`BeginPaint`. **Absent:** `FillRect`, `Rectangle`, `CreatePen`, `CreateSolidBrush`, `DrawText`,
`TextOut`, `Polygon`, `Ellipse`, `LineTo`. So `dcshell` fills rectangles via the standard
`ExtTextOutW(..., ETO_OPAQUE, &rect, L"", 0, ...)` idiom (paints `rect` with the bg color) and
does all text with `ExtTextOutW`. Stick to that palette when extending it.

## Build
```
shell\build-dcshell.bat        :: -> reference\shell-obj\dcshell.exe  (SH-4 0x1A6, subsystem 2.12)
```
Uses the vendored gweslab SH compiler + the **DC SDK** `inc`/`lib` (apps use the 2.12 SDK API,
not the CE3 leak headers). Links `coredll.lib` + `corelibc.lib` (the latter has
`WinMainCRTStartup`). Verified: links clean, 0 unresolved.

## Wire it into a bootable image
Test on a **stock 2.12 kernel** (boots fully; our from-source `nk.exe` is still mid-bring-up). For
*debuggable* boots use the **no-KD `nkscifkd`** (`reference\kernel-obj\nkscifkd.nokd.exe`, raw SCIF
text — see `../docs/08` §A′); for a clean run use `nknodbg.exe.stock`. Copy the chosen kernel over
`C:\wcedreamcast\release\retail\nknodbg.exe` (the `nk.exe` bib slot).

**Build-system gotcha (important):** `makeimg` *regenerates* `ce.bib` and `reginit.ini` every run by
merging the `IF`-guarded **sources** (`platform.bib`, `gemini.reg`, …) under the IMG/INIT env vars in
`toolchain\setenv.bat`. So editing `ce.bib` directly is futile (it gets overwritten) — edit the
source `.bib`/`.reg`, or flip an env flag. `gemini.reg` *is* a source (edits persist).

Two ways to get dcshell in + launched:
- **Quick (current):** overwrite the `sysstart.exe` slot with dcshell — it's already a module and is
  launched last by `"Launch60"="sysstart.exe"`. `cp reference\shell-obj\dcshell.exe
  C:\wcedreamcast\release\retail\OS\sysstart.exe` (original at `sysstart.exe.orig`). No bib/reg edits.
- **Proper:** add `dcshell.exe $(_FLATRELEASEDIR)$(RELEASEDIR_OS)dcshell.exe NK SH` to the MODULES
  block of the **source** `platform.bib`, copy the exe to `OS\`, and keep the `"Launch90"="dcshell.exe"`
  / `"Depend90"=hex:1e,00` (depend on gwes=0x1e) already in `gemini.reg [HKEY_LOCAL_MACHINE\init]`.

Drop the stock **devkit shell** (`shell.exe`, CESH — talks to the DA over P2 and faults with
`Exception 0x180 PC=ac00xxxx` in an emulator) via the SDK's own switch: `set IMGNOSHELL=1` in
`setenv.bat` (already set). It removes the `shell.exe`+`toolhelp.dll` modules and `"Launch11"="shell.exe"`.

Then: `toolchain\build-image.bat retail` → `wrap-image.ps1` → `make-gdi.ps1` → load
`reference\disc-shell\disc.gdi` in Flycast (`Debug.SerialConsoleEnabled` on).

## Reference
NT4 explorer source (sparse-cloned, gitignored) at
`vendor\nt4-ref\private\windows\shell\cabinet\` (`cabwnd.c`, `cabinet.c`, `base\`) — **read for
design** (taskbar/desktop/file-view logic) only; it can't be ported (full Win32/COM/shell32
namespace vs the CE subset — see `../docs/08`-adjacent analysis).

## Roadmap
v1 = launcher (done, builds). Next: icon grid + `BitBlt` icons, a window-list taskbar via
`EnumWindows`/`SetForegroundWindow`, a simple file browser (`FindFirstFile` tree), wallpaper via
`BitBlt`. All within the GDI subset above.
