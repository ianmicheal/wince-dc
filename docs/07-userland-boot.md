# Userland bring-up — debugging the from-source kernel on Flycast (2026-06-24)

This session drove the from-source `nk.exe` from "crashes loading the first DLL" to
"filesys fully up, launching the 2nd process, stuck on a cross-process call". Two real
**CE 2.12-vs-3.0 compatibility bugs** were found and fixed (we run a 3.0 kernel against the
stock 2.12 modules). Method: add `RETAILMSG` probes → run on Flycast → read the trace →
confirm the 2.12 truth from the SDK kernel in Ghidra (`get_struct_layout` / `read_memory` /
`get_xrefs_to`) → patch → rebuild → repeat.

Rebuild loop each iteration (retail):
`build-nklib` (or `build-oal` if HAL changed) → `build-nk` → copy `nk.exe` over
`C:\wcedreamcast\release\retail\nknodbg.exe` → `build-image` → `wrap-image.ps1`
(`-Out reference\0winceos.ours.bin`) → `make-gdi.ps1` → load `reference\disc-gdi\disc.gdi`
in Flycast.

## FIX 1 — `e32_rom` struct layout (2.12 ≠ 3.0)  [the big one]
**Symptom:** unhandled data-TLB-miss (`exc 0x040`) at `TEA=0x03d50000` in `DoImports+0x54`,
called from `LoadOneLibraryPart2`, loading **coredll**. `ProcessPageFault` → `LoaderPageIn`
→ `PageInModule` returned 0 ("could not find section") because `0x01d50000` is coredll's
header page, below its first o32 section.

**Root cause:** `DoImports` read `eptr->e32_unit[IMP] = {rva=0, size=0x31000}` — but coredll
(the base DLL) has **no imports** (`IMP={0,0}`). The size `0x31000` was actually 2.12's
`RES.rva`. Our 3.0 `e32_rom` struct (`ROMLDR.H`) had `e32_sect14rva`+`e32_sect14size` before
`e32_unit[]` and `e32_subsys` after — but the **2.12 `e32_rom` the SDK romimage writes** has
`e32_subsys` (ushort) at +24 and `e32_unit[]` at +28, total **100 bytes**, with **no sect14**.
The 3.0 struct shifted `e32_unit[]` by +4, so every directory entry was misread.
Confirmed 3 ways: live `DCDBG` trace, raw `e32_rom` bytes from our image (`subsys=9`=WINCE_GUI,
`EXP={0x27800,0x64c4}`, `IMP={0,0}` all align at +28), and Ghidra PDB `e32_rom` = exactly 100 B.

**Fix (KEEP — this is correct, not debug):**
- `vendor/sh-toolchain/ce3-oak/INC/ROMLDR.H` — `e32_rom` → 2.12 layout (`e32_subsys` ushort
  before `e32_unit[ROM_EXTRA]`; removed `e32_sect14rva`/`e32_sect14size`).
- `vendor/wince-src/.../NK/KERNEL/loader.c` (~1701) — `LoadE32` ROM branch sets
  `eptr->e32_sect14rva = 0; eptr->e32_sect14size = 0;` (2.12 ROM has no sect14).

**Effect:** coredll + filesys load, imports parse correctly.

## FIX 2 — API method-table hole `ProcMthds[4]` (2.12 ≠ 3.0)
**Symptom:** threads jump to `PC=0` via `APICall`→`ObjectCall` (`KPSLExceptionHandler`).
Probe in `OBJDISP.C` (`pfn = pci->ppfnMethods[iMethod]`) → **`'PROC':4` is NULL**.
**Root cause:** coredll (2.12) calls Process API method 4; our 3.0 `ProcMthds[4] = (PFNVOID)0`
(3.0 removed it). Read the 2.12 `ProcMthds` from Ghidra (xref to `SC_ProcTerminate@8c0156d0`
→ table @ `8c002220` → `read_memory`): identical to ours **except [4] = `SC_ProcGetIndex`**.
**Fix (KEEP):** `schedule.c` `ProcMthds[4] = (PFNVOID)SC_ProcGetIndex` (the function already
exists in `kmisc.obj`; 3.0 only dropped the table entry — do NOT add a body, that's a dup → LNK2005).

## CURRENT FRONTIER — cross-process `PerformCallBack`
After fix 2 (rebuilt; confirmed): **filesys fully initialises** — registers its API set
(`APIS:2`=RegisterAPISet), calls `Wn32:102`=`SC_SignalStarted` (→ `SetEvent(hSignalApp)`),
then `Sleep`+`WaitForMultiple` = its idle service loop (NORMAL). `RunApps` wakes and launches
a **2nd process (`p2`)**. `p2`'s only traced call is **`Wn32:113` = `-1` = `PerformCallBack`**
(cross-process server call), then the system goes fully idle (`[TMR]` keeps ticking → timer +
scheduler fine; everything just blocked).

`PerformCallBack` (in `ObjectCall`, the `pfn==-1` path) **migrates the calling thread into the
target process** (`pth->pProc = pprc; SetCPUASID(pth)`) and runs the callback there. `SetCPUASID`
works (normal scheduling uses it), so `p2` likely migrated and is blocked — or the callback
target is bad. **Last probe added (awaiting next Flycast run):** in `OBJDISP.C`,
`DCDBG CB p%d->p%d pfn=%8.8lx kpsl=%d` prints `p2 → target-proc` + the callback fn. Interpret:
`->p0` = into kernel proc (gated out of the `OC` trace); `->p1` = into filesys; `pfn=0/wild` =
bad target. Continue from that line.

## DCDBG probes currently in the tree (STRIP once boot is stable)
All are `RETAILMSG(1,...)` / `OEMWriteDebugString` and gated to be low-noise:
- `loader.c` `DoImports` — `DCDBG DoImp base/imp.rva/imp.sz/oc/s0*`
- `loader.c` `PageInModule` not-found — `DCDBG PIM-NF ...`
- `virtmem.c` `ProcessPageFault` fail path — `PPF-FAIL a/s/B/fl/pgr/alk/ak/aP/acc`
- `schedule.c` `SystemStartupFunc` — `DCDBG DLLrgn first/last/base/Z`
- `OBJDISP.C` `ObjectCall` — `OC p%d set:method` (gated `procnum != 0`) + `OC-NULL ...` +
  `DCDBG CB p%d->p%d pfn ...`
- `PLATFORM/DREAMCAST/KERNEL/HAL/timer.c` `Timer0ISR` — `[TMR]` heartbeat every 64 ticks
(These are debug only. The FIX-1/FIX-2 source changes above are NOT debug — keep them.)

## Win32 method numbers (from `KWIN32.C`, for reading `OC` traces)
2=CreateAPISet 3=VirtualAlloc 25=FindResource 26=LoadResource 52=CreateEvent 53=CreateProc
58=WaitForMultiple 68=GetCallerProcess 78=CreateCrit 82=Sleep 99=? 102=SignalStarted
113=`-1`=PerformCallBack 118=DebugNotify. Handle sets: `PROC`(SH_CURPROC) `THRD` `EVNT` `APIS`.

## Method to continue (per OC-NULL or hang)
1. Run `disc.gdi` in Flycast, read the `DCDBG`/`OC` tail.
2. `OC-NULL '<set>':N` → Ghidra: `get_xrefs_to` a known method of that set → DATA xref = the
   2.12 table base → `read_memory` → diff vs our `<set>Mthds[]` in `schedule.c`/`kwin32.c` →
   restore the missing `SC_*` (the function usually already exists; just the table slot was zeroed).
3. Struct mismatch → Ghidra `get_struct_layout <name>` for the authoritative 2.12 layout; fix
   ours to match (we run 2.12 modules → kernel must use 2.12 on-disk struct layouts).
4. Hang → trace API calls (`OC`), find the last call before idle; if it's a cross-process call,
   see `08-emulator-debugging.md` (consider WinDBG-in-Flycast for real breakpoints).

## State of the world (housekeeping)
- `C:\wcedreamcast\release\retail\nknodbg.exe` is currently **our** `nk.exe` (stock saved at
  `nknodbg.exe.stock` — restore for a stock image).
- `utils\ip.bin` = the SDK's `tools\GDWorkshop\ip_drago.bin` (bootfile already `0WINCEOS.BIN`).
  It's gitignored → re-copy on a fresh clone (`bootstrap` could automate this).
- Build outputs (`reference\*.bin`, `kernel-obj\`) are gitignored — rebuild on the other PC.
