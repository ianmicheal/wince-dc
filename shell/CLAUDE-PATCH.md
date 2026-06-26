# CLAUDE-PATCH.md — additions to merge into the top-level `CLAUDE.md`

The shell/DCWin desktop is a new subsystem not yet recorded in the root `CLAUDE.md`.
Apply the edits below when you next touch the root doc. (Kept separate so the root file
stays the kernel/image source of truth until merged.)

---

## 1. Add to "## Current state (resume here)"

```
- ✅ **Windowed desktop shell (DCWin) — RUNS on Flycast.** CE never shipped a windowed
  shell on the DC, so we wrote one as a **compositor**: `dcshell.exe` owns a fullscreen
  DirectDraw primary; client apps (`dcwcalc`/`dcwclock`/`dcwexp`) are separate processes
  publishing per-window draw-command lists into a named shared section; the shell
  composites them into real windows (move/resize pending). NT4-ish chrome, desktop icons,
  Start menu, taskbar, Tab focus-cycle, file-type icons, launch-from-CD-ROM. Input is
  **DirectInput**: polled keyboard (low-latency) + the controller as a software pointer
  (analog→cursor, A/B/X/Y→click). Source + docs in `shell/` (`HANDOFF.md`, `BUILD.md`,
  `ROADMAP.md`). Built against the **stock 2.12 SDK** API (not the CE3 leak headers);
  runs on the stock kernel.
```

## 2. Add to "## Layout"

```
- `shell/` — DCWin windowed desktop (separate from the kernel work). `dcshell.c`+`dcgfx`
  (compositor + DDraw display), `dcwin.h` (shared protocol), `dcwlib`+`dcw*.c` (client
  apps), `dcinput` (DirectInput kbd+pointer). Build: `shell\build-dcshell.bat <flavor>`.
  Docs: `shell/HANDOFF.md` (start here), `BUILD.md`, `ROADMAP.md`.
```

## 3. Add a "## DC platform facts (shell)" section (or fold into Conventions)

These are non-obvious and have cost real debugging time:

```
- **DC display = DirectDraw, NOT GDI.** No GDI desktop primary (`GetDC(NULL)==0`). Pixels
  appear only via a fullscreen exclusive DDraw primary + `SetDisplayMode(640,480,16)`. The
  primary is VOLATILE — Blt a back buffer every frame or it goes black when idle.
- **GWES GDI is a text+blit subset with PHANTOM exports.** Real: ExtTextOutW, SetBk/TextColor,
  SetBkMode, GetStockObject, CreateFontIndirectW, SelectObject, CreateCompatibleDC, DeleteDC,
  DeleteObject, BitBlt. PHANTOM (import lib lists them; calling wild-jumps to PC=0x01be0000):
  FillRect, PatBlt, SetPixel, CreateSolidBrush, CreatePen, Rectangle, Ellipse, Polyline,
  CreateCompatibleBitmap, CreateDIBSection, MaskBlt, StretchBlt/DIBits, TransparentImage.
  Do fills/icons via DDraw (COLORFILL Blt, Lock + color-key Blt); SEH-probe any new GDI call.
- **Cross-process IPC**: named CreateFileMapping section maps at the SAME VA (0x42000000) in
  every process → shared pointer structs work. No `SetEvent` on this coredll (kernel uses
  EventModify) → IPC is poll-based.
- **coredll resolves imports by ORDINAL** — link the lib flavor (retail/debug) matching the
  image's coredll.dll or GDI/DDraw imports mis-resolve into wild jumps.
```

## 4. Add to "## Build / commands" (shell loop)

```
:: --- DCWin shell (separate from kernel) ---
shell\build-dcshell.bat retail         :: -> reference\shell-obj\{dcshell,dcwcalc,dcwclock,dcwexp}.exe
:: copy the 4 exes to C:\wcedreamcast\release\retail\OS\ , then:
build-image.bat retail  +  wrap-image.ps1  +  make-gdi.ps1   :: -> reference\disc\disc.gdi
:: shell launch is wired via gemini.reg [HKLM\init] "Autorun"="dcshell.exe" (sysstart runs it)
:: setenv.bat: IMGNOSHELL=1 (drop devkit CESH), IMGDIRECTINPUT=1 (dinput.dll in image)
```

## 5. Note on `make-gdi.ps1` default paths (fixed)

The repo moved to `C:\dev\Dreamcast\wince-dc`; `make-gdi.ps1` param defaults were hardcoded
to the old `C:\Dev\wince-dc` and have been corrected (Image/OutDir/Utils). OutDir now
defaults to `reference\disc` (the single fixed disc output — never per-iteration folders).
If another `toolchain\*.ps1` script can't find a tool, check for the same stale path.
