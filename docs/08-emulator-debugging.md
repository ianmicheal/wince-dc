# Debugging WinCE on the Dreamcast — Flycast logging + WinDBG (research, 2026-06-24)

How to get debug output (and potentially a real debugger) for the WinCE kernel running in
**Flycast on Windows**. Covers both our from-source kernel and the stock Sega kernels.

## The transport landscape
- **Sega devkit (real HW):** Dev.Box / GD-Emulator + **Debug Adapter**, connected to the PC
  over **SCSI**. The DA/SCSI link multiplexes WinDBG-KD + the eVC Platform Manager (app
  download) + the CESH shell. Documented in `C:\wcedreamcast\docs\relnotes.htm`.
- **Retail console / emulator:** no SCSI/DA. Exposes the SH-4 **SCIF** (on-board serial, the
  "coder's cable" / dcload-serial pads) and — on later units — the **BBA** (RTL8139 Ethernet).

## The three stock kernel variants (`release\{retail,debug}\`)
- `nk.exe` — full debug kernel, debug I/O via the **DA** (no SCIF). In Flycast (no DA) → silent.
- `nknodbg.exe` — no kernel debugger, debug I/O via the **DA** → silent in Flycast.
- `nkscifkd.exe` — KD + debug over the **SH-4 SCIF**. CONFIRMED: it references the SCIF regs
  `0xFFE80000` (retail 3, debug 2); `nk.exe` references **0**. BUT its SCIF traffic is the
  **WinDBG KD packet protocol** (binary), not text — with no WinDBG attached it just spews KD
  handshake bytes → "garbled ascii" in the console.
So **no stock kernel emits clean text in Flycast** without help.

## Flycast SCIF reality on Windows (decisive)
Source: `core/hw/sh4/modules/serial.cpp` (the SCIF) + the `PTYPipe` it attaches when
`config::SerialConsole` (`Debug.SerialConsoleEnabled`) is on.
- Flycast emulates the SCIF fully and bidirectionally (`pipe->write` on TX, `pipe->read`/
  `available` on RX). The `Pipe` interface is `core/hw/hwreg.h:298`. Out of the box only **NAOMI**
  peripherals attach a pipe (card readers etc.); the **Dreamcast SCIF has no host bridge** except
  `SerialConsole`.
- `PTYPipe` is **asymmetric by OS**:
  - **Windows:** `init()` does `AllocConsole()` and dup2's SCIF-TX → **stdout console window**.
    `available()` is guarded `#if __unix__||__APPLE__` → **always 0 → RX is dead. TX-only.**
  - **Linux/macOS (`SerialPTY`):** opens `/dev/ptmx` → a **bidirectional pseudoterminal**
    ("Pseudoterminal is at /dev/pts/N").
- Consequence on **Windows Flycast**: you can SEE SCIF bytes (this is how our kernel's printf
  shows up) but cannot SEND. So WinDBG-KD (needs RX) **cannot work as-is on Windows**, and
  `nkscifkd` output is unusable garbage. → you must patch the kernel for clean text, OR patch
  Flycast for a bidirectional endpoint.

## BBA/TCP app-download — NOT viable on this SDK
The 2.12 image ships **modem/PPP only** (`mppp`, `microstk`, `serial`, `serial.dll`) and **no
RTL8139/BBA NDIS driver**. So the Ethernet/TCP eVC-download path doesn't exist without writing a
BBA miniport. Drop it. In an emulator the practical "deploy" is just `makeimg` + reboot the GDI.

## Two real paths to debug in Flycast

### A. Patch the kernel for clean SCIF text logging (quick, one-way)
Redirect the kernel's debug output to raw SCIF bytes (Flycast's `SerialConsole` then shows them).
This is exactly our `PLATFORM\DREAMCAST\KERNEL\HAL\debug.c` `OEMWriteDebugByte` (poll
`SCFSR2.TDFE`, write `SCFTDR2`, base `0xFFE80000`).
- For a **stock** kernel: binary-patch its `OEMWriteDebugByte` (we have symbols — Ghidra
  `OEMInitDebugSerial`/`OEMWriteDebug*` in the SDK kernel) to the ~20-byte SCIF write, then
  makeimg. Gives readable stock-kernel boot logs in Flycast.
- For **our** kernel: already done (clean SCIF). This is what we use today.

### B. WinDBG in Flycast (powerful — symbolic kernel debugging, ~50 lines of Flycast)
`nkscifkd.exe` already speaks KD over SCIF; the only Windows gap is a **bidirectional** SCIF
endpoint. Implement a `SerialPort::Pipe` subclass over a **TCP socket** (or named pipe) and
attach it to the SCIF for Dreamcast mode (instead of the `AllocConsole` stdout in `PTYPipe`).
Then:
1. run `nkscifkd` in patched Flycast,
2. bridge the socket to a COM port (`com0com`) or a KD-over-TCP shim,
3. point `C:\wcedreamcast\tools\windbg.exe` + `dmkdx86.dll` (the KD transport; has baud/COM
   options) at it; match the baud `nkscifkd` programs into `SCBRR2`.
→ real breakpoints / memory / symbols on the **stock Sega kernel** in the emulator — no DA, no
hardware. (Linux/macOS Flycast already has the bidirectional PTY; bridge that to WinDBG.)

**Recommendation:** for the RE/porting work, **B** is worth the small Flycast patch — WinDBG
with the SDK symbols beats printf-spelunking the 2.12 kernel. Not yet done; the `nkscifkd` SCIF
baud (`SCBRR2`) still needs decoding (import `nkscifkd.exe` into Ghidra — it has COFF symbols
like `nknodbg`) to line up the WinDBG side.

## Quick reference
- SCIF base (SH7750): `0xFFE80000` (SCSMR2 +0, SCBRR2 +4, SCSCR2 +8, SCFTDR2 +C, SCFSR2 +10,
  SCFRDR2 +14, SCFCR2 +18, SCFDR2 +1C). Our `debug.c` uses 8N1 @ 57600.
- Flycast serial: `core/hw/sh4/modules/serial.cpp`, `Pipe` iface `core/hw/hwreg.h:298`,
  option `Debug.SerialConsoleEnabled` (+ `SerialPTY` on POSIX).
- Host KD tools (SDK `tools\`): `windbg.exe`, `kdxwce.dll`, `dmkdx86.dll` (KD transport, serial).
