# HANDOFF.md — resume the DC shell on another PC

Read this first, then `BUILD.md` (how to build) and `ROADMAP.md` (what's left).
The top-level `CLAUDE.md` covers the kernel/image project; this file covers the **shell**.

## What this is

`dcshell.exe` is a windowed, multitasking desktop for Dreamcast Windows CE 2.12. CE never
shipped a windowed shell on the DC, so we wrote one as a **compositor**: the shell owns the
display; client apps are **separate processes** that publish per-window draw-command lists
into a shared memory section; the shell composites them into windows. Real preemptive
multitasking (the stock kernel already runs many processes) + our window server.

## The two hard-won facts that shape everything

1. **DC display = DirectDraw, not GDI.** There is NO GDI desktop primary
   (`GetDC(NULL)` returns 0). Pixels only appear via a fullscreen exclusive DirectDraw
   primary + `SetDisplayMode(640,480,16)`. The primary is **volatile** — you must Blt a
   back buffer to it **every frame** or it goes black when idle. GDI text/blit works only
   on a DC obtained from a DDraw surface (`surface->GetDC`).

2. **GWES GDI is a text+blit SUBSET with PHANTOM exports.** Real (verified via SEH probe):
   `ExtTextOutW, SetBkColor, SetTextColor, SetBkMode, GetStockObject, CreateFontIndirectW,
   SelectObject, CreateCompatibleDC, DeleteDC, DeleteObject, BitBlt`. **Phantom** (the
   import lib lists them, but calling them wild-jumps to `PC=0x01be0000`):
   `FillRect, PatBlt, SetPixel, CreateSolidBrush, CreatePen, Polyline, Rectangle, Ellipse,
   CreateBitmap, CreateCompatibleBitmap, CreateDIBSection, TransparentImage, MaskBlt,
   StretchBlt, StretchDIBits`. So: **fills/bevels/icons via DirectDraw** (COLORFILL Blt,
   Lock + color-key Blt), **text via ExtTextOutW** on a surface DC. SEH-probe any new GDI
   call before trusting it.

## Files (all in `shell/`)

| File | Role |
|---|---|
| `dcgfx.h/.c` | Display layer: DDraw primary + VRAM back buffer, COLORFILL fills, bevels, Arial fonts, 16/32px color-keyed icons, `GfxPresent` (returns TRUE if surface was lost+restored), `GfxLaunch` (display hand-off for fullscreen apps), `GfxIcon`/`GfxIconBig`. Icon art = ASCII in `s_iconArt`, magenta `0xF81F` = transparent key. |
| `dcwin.h` | **Shared protocol** (shell ↔ clients). `DcShared{magic,execSeq,execPath,win[4]}`; `DcWindow{inUse,ownerPid,x,y,w,h,title,icon,wantClose,gen,cmdCount,cmd[48],inHead,inTail,in[16]}`; `DcCmd{op,x,y,w,h,color,color2,text}`. Ops: NONE/FILL/TEXT/ICON. Icon ids `ICON_COMPUTER..ICON_FILE`, `ICON_SWIRL=6`, `ICON_CURSOR=7`, `ICON_COUNT=8`. |
| `dcwlib.h/.c` | Client lib: `DCWinOpen/BeginFrame/Fill/Text/Icon/EndFrame` (seqlock), `DCWinPollKey`, `DCWinShouldClose`, `DCWinExec` (ask shell to launch a path), `DCWinClose`. |
| `dcwcalc.c` / `dcwclock.c` / `dcwexp.c` | Client apps: Calculator, Clock, windowed Explorer (file browser; routes launches to the shell via `DCWinExec`). |
| `dcshell.c` | The desktop + compositor. Shared section, launch logic (`IsDcwApp`/`ShellLaunch`: `dcw*` → windowed, else fullscreen via `GfxLaunch`), focus, desktop icon grid + Start menu + taskbar, per-layer `Render`, `OnKey`, `HandleClick`, message loop. |
| `dcinput.h/.c` | DirectInput layer: polled keyboard (DIK→VK, edge + auto-repeat) and controller pointer (analog→cursor, A/B/X/Y→click). |
| `build-dcshell.bat` | Builds all of the above (flavor param). |

## Key mechanisms

- **Cross-process shared memory**: `CreateFileMappingW((HANDLE)-1, ... , DCWIN_SECTION)` +
  `MapViewOfFile`. CE maps the named section at the **same VA (0x42000000) in every
  process**, so pointer structs are valid cross-process. Poll-based IPC (no `SetEvent`).
- **gen seqlock**: client bumps `gen` odd while writing a frame, even when stable; the
  shell snapshots a window's commands only when `gen` is even and unchanged across the read
  (no torn frames).
- **Display ownership / launch hand-off**: DDraw fullscreen is EXCLUSIVE. To run a
  fullscreen app the shell releases DDraw → `CreateProcess` → `WaitForSingleObject(INFINITE)`
  → reclaim. Windowed `dcw*` apps launch non-blocking and are composited.
- **Render-on-change loop**: render only when something changed (input / clock tick / a
  client published a new gen / cursor moved / surface restored), but **present every frame**
  (`Sleep(16)`).
- **Input (current)**: `DInUpdate()` each frame → drain `DInNextKey()` into `OnKey()`;
  `DInCursor()` for the pointer; `DInTookClick()` → `HandleClick(x,y)`. `WM_KEYDOWN` is a
  fallback only when the DI keyboard didn't acquire (`s_diKbd`).

## Current state / where we stopped

- **Just finished**: rewrote input to DirectInput (keyboard latency fix) + added a
  controller-driven software pointer. `dcinput.{h,c}` written; integrated into `dcshell.c`
  (`DInInit` after `GfxInit`, loop polling, `HandleClick` hit-testing, `ICON_CURSOR`
  drawn on top); `build-dcshell.bat` compiles `dcinput.c` and links `dinput.lib` +
  `dxguid.lib`. **Built retail clean** (no unresolved externals), deployed to
  `release\retail\OS\`, rebuilt image + wrapped + burned `reference\disc\disc.gdi`.
  **Not yet tested on HW/Flycast** — that's the immediate next step (read `DCIN:` logs).
- `IMGDIRECTINPUT=1` is set in `setenv.bat`, so `dinput.dll` is in the image.

## Immediate next step

Load `reference\disc\disc.gdi` in Flycast (SerialConsole on). Confirm:
`DCSHELL: DI keyboard active`, `DCIN: keyboard acquired`, `DCIN: joystick acquired`. Move
the stick (cursor should glide), press A/B/X/Y (clicks Start/taskbar/icon/window-X). If the
pointer/keys misbehave, tune the DI constants in `dcinput.c` (deadzone 150, speed `/80`,
range ±1000, button indices 0..3). Then proceed down `ROADMAP.md` (HL crash → window
drag → resize).

## Gotchas that already bit us (don't repeat)

- Don't erase the window in `WM_PAINT`/`WM_ERASEBKGND` (it IS the primary) — `hbrBackground=NULL`,
  return 1 from ERASEBKGND, just `ValidateRect` in PAINT.
- Match lib flavor to kernel coredll (ordinal resolution).
- `lstrcpynW` not exported → use the `CopyN` helper. `SetEvent` absent → poll.
- Per-layer compositing (fills+text per window, back-to-front) — a two-pass
  all-fills-then-all-text renderer paints non-focused text over focused fills.
