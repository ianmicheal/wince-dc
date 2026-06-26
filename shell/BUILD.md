# BUILD.md — building & testing the DC shell (DCWin)

Everything here runs on Windows. Paths assume the repo at `C:\dev\Dreamcast\wince-dc`
and the DC SDK at `C:\wcedreamcast` (override with `WCEDREAMCASTROOT`). On a fresh PC,
clone the repo and place the SDK first (see top-level `CLAUDE.md` §Setup).

## TL;DR — one full iteration (retail)

```bat
:: 1. compile shell + clients (SH-4)            -> reference\shell-obj\*.exe
shell\build-dcshell.bat retail

:: 2. copy the 4 exes into the image's OS dir
copy /Y reference\shell-obj\dcshell.exe  C:\wcedreamcast\release\retail\OS\
copy /Y reference\shell-obj\dcwcalc.exe  C:\wcedreamcast\release\retail\OS\
copy /Y reference\shell-obj\dcwclock.exe C:\wcedreamcast\release\retail\OS\
copy /Y reference\shell-obj\dcwexp.exe   C:\wcedreamcast\release\retail\OS\

:: 3. rebuild image -> wrap -> burn GDI
toolchain\build-image.bat retail
powershell -File toolchain\wrap-image.ps1 -NkBin C:\wcedreamcast\release\retail\NK.bin -Out C:\wcedreamcast\release\retail\0winceos.bin
powershell -File toolchain\make-gdi.ps1   -Image C:\wcedreamcast\release\retail\0winceos.bin

:: 4. load reference\disc\disc.gdi in Flycast (SerialConsole on)
```

After the make-gdi default fix, step 3's `make-gdi.ps1` needs no extra flags — it
writes to `reference\disc\` using `utils\buildgdi.exe`. **Always build to
`reference\disc\` — never per-iteration folders** (Flycast points there).

## Flavor: retail vs debug — MUST MATCH THE KERNEL

CE resolves `coredll` imports **by ordinal**, so linking retail libs against a debug
`coredll.dll` (or vice-versa) silently mis-resolves GDI/DDraw imports → wild jump.
`build-dcshell.bat <flavor>` selects `C:\wcedreamcast\lib\<flavor>`. The image tree
under `release\<flavor>\` carries the matching `coredll.dll`. **Current working build
is `retail`.** Use `debug` only when you need `DEBUGMSG`/`RETAILMSG` from the kernel
(retail compiles them out) — then build everything `debug` and burn the `release\debug`
tree.

## What gets built

`build-dcshell.bat`:
- `dcgfx.c` + `dcshell.c` + `dcinput.c` → **dcshell.exe** (the compositor/desktop).
  Links `coredll.lib corelibc.lib ddraw.lib dinput.lib dxguid.lib`.
- `dcwlib.c` + `dcwcalc.c` → **dcwcalc.exe**   (client)
- `dcwlib.c` + `dcwclock.c` → **dcwclock.exe** (client)
- `dcwlib.c` + `dcwexp.c` → **dcwexp.exe**    (client; windowed Explorer)

Clients link only `coredll.lib corelibc.lib` (they draw via the shared section, not
GDI/DDraw directly). `corelibc.lib` supplies `WinMainCRTStartup`.

Compile flags (all): `-DUNDER_CE=212 -D_WIN32_WCE=212 -DUNICODE -D_UNICODE -DSH4=1 -DSHx=1`.

## Image wiring (already in place; for reference)

Lives in the SDK tree under `C:\wcedreamcast\release\{retail,debug}\` (NOT in the repo):
- `platform.bib` MODULES block has the 4 exes:
  `dcshell.exe $(_FLATRELEASEDIR)$(RELEASEDIR_OS)dcshell.exe NK SH` (+ dcwcalc/dcwclock/dcwexp).
- `gemini.reg` `[HKEY_LOCAL_MACHINE\init]` `"Autorun"="dcshell.exe"` — `sysstart.exe`
  reads this and launches the shell last.
- `toolchain\setenv.bat` sets `IMGNOSHELL=1` (drops the devkit CESH `shell.exe` that
  faults in emulation), `IMGNODEBUGGER=1`, and **`IMGDIRECTINPUT=1`** (so `dinput.dll`
  is in the image — required for the DI keyboard/controller).
- `nknodbg.exe` = the kernel in the `nk.exe` bib slot. For clean shell runs the patched
  no-KD SCIF logger is used; stock kernel saved at `nknodbg.exe.stock`.

`makeimg` **regenerates** `ce.bib`/`reginit.ini` from the `IF`-guarded sources every run —
edit `platform.bib`/`gemini.reg`/`setenv.bat`, never `ce.bib` directly.

## Optional: stage Half-Life DC onto the disc (\CD-ROM)

```bat
powershell -File toolchain\make-gdi.ps1 -Image C:\wcedreamcast\release\retail\0winceos.bin -ExtraData C:\Games\hldc\data
```
`-ExtraData <folder>` robocopies the folder's contents into the ISO data dir, mounted as
`\CD-ROM` on the DC. `HALFLIFE_DC.EXE` launches (fullscreen, via display hand-off) but
crashes at level load — see ROADMAP. Skip `-ExtraData` for fast shell iteration (the HL
track makes the ISO ~1.1 GB and slows the burn).

## Serial logs to watch (Flycast SerialConsole)

- `DCSHELL: desktop up` — shell reached its message loop.
- `DCSHELL: DI keyboard active` / `... WM fallback` — whether polled DI keyboard took over.
- `DCIN: keyboard acquired` / `DCIN: joystick acquired` — which DI devices enumerated.
- `DCSHELL: launching <path>` — a launch request (icon/Start/Explorer) fired.

## Common failure → cause

| Symptom | Cause / fix |
|---|---|
| Black screen, only moving things visible | DC primary is VOLATILE — must Blt back buffer every frame (`GfxPresent`). Already handled. |
| Wild jump to `PC=0x01be0000` | Called a **phantom** GDI export (FillRect/Rectangle/SetPixel/CreateSolidBrush/CreateCompatibleBitmap/…). Use DDraw instead. SEH-probe before trusting any new GDI call. |
| Teal-only / lost surface | VRAM surface lost; `GfxPresent` returns TRUE on restore so caller re-renders. |
| GDI/DDraw imports resolve to garbage | retail libs vs debug DLL ordinal mismatch — match flavor. |
| `make-gdi.ps1: buildgdi.exe not found` | stale path; pass `-Utils C:\dev\Dreamcast\wince-dc\utils` (defaults now fixed). |
| Shell never launches | check `gemini.reg` `"Autorun"`, and that `sysstart.exe` is present + last. |
