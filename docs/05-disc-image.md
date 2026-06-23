# Disc image — booting the from-source kernel in Flycast (CDI)

Turns our wrapped `0winceos.bin` into a bootable Dreamcast **CDI** (Padus DiscJuggler)
that Flycast/real HW can load. Driver: `toolchain\make-disc.ps1`.

## Boot model (from the SDK's own scripts)
- `ip_drago.bin` (GD Workshop) is the WinCE **IP.BIN** — its bootfile field @0x60 is
  literally `0WINCEOS.BIN`. The DC BIOS runs IP.BIN, which loads `0WINCEOS.BIN` from the
  ISO9660 root. (rename `ip_drago.bin` → `IP.BIN`.)
- `makecd.bat` shows the disc tree: `0winceos.bin` at the **GD root** (must be the first
  file alphabetically) + OS modules under `\WinCE\`. For a kernel smoke-boot, only the boot
  file is required.
- `gore.bat` = `dumpnk nk.bin nk.raw` → `gore2 nk.raw 0winceos.bin`; our `wrap-image.ps1`
  reimplements `gore2` (byte-identical header).

## Tools
- **mkisofs** (cdrtools 3.00, cygwin build) — builds the ISO and injects IP.BIN via `-G`.
  Not in the DC SDK; pulled to `c:\dev\cdrtools\` (mkisofs.exe + cygwin1/cygiconv-2/cygintl-8
  DLLs) from the `esc0rtd3w/dreamcast-tools` selfboot mirror.
- **cdi4dc** (img4dc 0.3b, by SiZiOUS) at `c:\dev\cdi4dc\cdi4dc.exe` — ISO → CDI.

## Recipe (the "Marcus tutorial" / img4dc flow)
```
mkisofs -C 0,11702 -V WINCE -G IP.BIN -joliet -rock -l -o wince.iso <srcdir>
cdi4dc  wince.iso  wince.cdi
```
- `-G IP.BIN` embeds the 0x8000-byte bootsector (the renamed `ip_drago.bin`).
- `-C 0,11702` sets the high-density session offset cdi4dc expects (11702 = DC pregap).
  (mkisofs warns "-C without -M" — harmless for selfboot.)
- `<srcdir>` contains `0WINCEOS.BIN` (our wrapped image) at its root.

## One-shot
```
powershell -File toolchain\make-disc.ps1 -Image reference\0winceos.ours.bin -Cdi
```
Stages `reference\disc\cd\0WINCEOS.BIN` + `IP.BIN`, runs mkisofs + cdi4dc →
`reference\disc\wince.cdi`. Add `-WithModules` to also stage `\WinCE\` OS modules.
Load `wince.cdi` in Flycast.

## Status / caveats
- The CDI boots **our from-source CE 3.0 kernel** (`nk.exe` → makeimg → `0winceos.bin`).
- The kernel's OAL/CRT still has bring-up **stubs** (fixed-time RTC, `OEMIoControl`→FALSE,
  ISR/soft-float stubs) and the user-mode modules are stock 2.12 (ABI-mismatched with a 3.0
  kernel), so expect the kernel to start (watch the SCIF console) but not bring up a full
  shell yet. Watch Flycast's serial output for our OEMInit/`OEMWriteDebugString` traffic.
- For CDI the boot LBA lives in IP.BIN; cdi4dc handles the layout. If you switch to a GDI
  flow, the bootfile LBA must match the high-density track base (45000).
