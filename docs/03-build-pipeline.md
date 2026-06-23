# Build Pipeline — VALIDATED end-to-end (2026-06-23)

Ran the DC SDK's own `makeimg` round-trip with **zero** Platform Builder and **no compiler**.
It reproduced a canonical CE ROM identical in structure to the shipped image. The full chain
is now understood and reproducible.

## The chain
```
 (leaked SHX source) --[gweslab SH cl.exe]--> nknodbg.exe        # only when modifying the kernel
       OS module .exe/.dll  (already in release\retail\OS, CoreOS_*)
                    |
   release\retail\*.bib + *.reg + *.dat  --[ makeimg ]-->  NK.bin              (B000FF CE ROM)
                    |                                          |
                    |                                   [ DUMPNK NK.bin raw ]
                    |                                          v
                    |                                   raw memory image  (loads @ 0x8C010000)
                    |                                          |
                    |                            [ prepend 0x800 Sega header + pad ]
                    v                                          v
              boot launch order (.reg)                  0winceos.bin   (DC-loadable)
                                                               |
                                                  [ bingen / IP.BIN / GD Workshop ]
                                                               v
                                                     bootable GD-R / dc-load image
```

## What the round-trip proved
- `makeimg` (env from `tools\build_retail.bat`, a faithful replica of `wce.bat retail`) →
  `release\retail\NK.bin`: **29 modules**, `Physical start 8C010000`, ROM span `0x1CC43C`.
- `NK.bin` is the standard **`B000FF`** CE ROM; header encodes start `8C010000` + len `0x1CC43C`.
- **`DUMPNK NK.bin out`** parses it cleanly (it *rejected* the shipped `0winceos.bin`) and emits
  the 1,885,244-byte raw memory image. ROMHDR `ECEC` at offset **0x40**.
- Shipped `0winceos.bin`: ROMHDR `ECEC` at file offset **0x840** = 0x800 header + 0x40. ⇒
  **`0winceos.bin = [0x800 hdr] + [raw memimage padded 0x1CC43C→0x1CC800]`**. Exact match.

## The 0x800 Sega wrapper header (offsets in the .bin)
| off  | bytes (LE)     | meaning |
|------|----------------|---------|
| 0x00 | `D6 1A`        | magic |
| 0x14 | `00 00 01 0C`  | load base = **0x0C010000** (physical RAM; cached mirror 0x8C010000) |
| 0x1C | `00 C8 1C 00`  | payload length = **0x1CC800** (raw image padded up to 0x800 boundary) |
| 0x20 | `00 00 01 0C`  | (repeat of base) |
| 0x800| payload starts | raw memory image (B000FF stripped → flat, as DUMPNK emits) |

## Image memory map (from `ce.bib`)
```
NK       8C010000  005F0000  RAMIMAGE   ; kernel + 29 modules
RAM      8C600000  009D0000  RAM
DRIVGLOB 8CFD0000  00030000  RESERVED   ; Maple/ATAPI/D3DHAL/GDI globals
CONFIG: AUTOSIZE=ON COMPRESSION=ON KERNELFIXUPS=ON ROMFLAGS=1
```

## Reproduce
```
toolchain\build-image.bat retail                      # env + makeimg -> NK.bin
powershell -File toolchain\wrap-image.ps1 ^
  -NkBin C:\wcedreamcast\release\retail\NK.bin ^
  -Out   C:\wcedreamcast\release\retail\0winceos.bin  # DUMPNK + 0x800 header -> bootable
```
`wrap-image.ps1` was verified to reproduce the shipped 0x800 header **byte-for-byte** and the
exact 1,888,256-byte total. Artifacts + SHA-256 in `reference/MANIFEST.md`.

## Next: swap in a leaked-source kernel
1. Compile the leaked `WINCEOS\COREOS\NK` (SHX) with the gweslab SH `cl.exe` → `nknodbg.exe`
   (start: `toolchain\build-kernel.bat retail` — kernel compile smoke).
2. Drop it over `release\retail\nknodbg.exe` (what `ce.bib` line 22 pulls in as `nk.exe`).
3. Re-run `build-image.bat` → new `NK.bin` → `wrap-image.ps1` → boot. Only the kernel changed;
   the 28 other modules stay the stock 2.12 binaries.
