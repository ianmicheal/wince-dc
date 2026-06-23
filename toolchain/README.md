# Toolchain — WinCE-on-Dreamcast (Path B)

All scripts assume the four paths at the top of `setenv.bat` are correct:

| var | default | what |
|-----|---------|------|
| `DCSDK`    | `C:\wcedreamcast` | Sega "Dragon" CE 2.12 SDK: makeimg, modules, .bib/.reg |
| `WINCESRC` | `C:\dev\Dreamcast\wince-src` | leaked CE 3.0 source (WINCE300 branch) |
| `GWESLAB`  | `C:\dev\Dreamcast\vendor\WindowsCE-Build-Tools` | gweslab SH `cl.exe` (SH4) |
| `WCE212`   | `C:\Windows CE Tools\wce212\bin` | authentic 2.12 `SHCL.EXE` (fallback) |

## Compilers (both validated on this host)
- **gweslab `cl.exe`** — "MS C/C++ Optimizing Compiler 13.10.3343 for Renesas SH",
  **defaults to SH-4** (`dumpbin` → `1A6 machine (SH4)`). **Primary.**
- **wce212 `SHCL.EXE`** — "MS 32-bit C/C++ Compiler 12.01.8511 for Hitachi SH" (the
  exact 2.12 vintage). Defaults to SH-3; SH-4 flag TBD. Authentic fallback.

## Scripts
| script | does |
|--------|------|
| `setenv.bat [retail\|debug]` | sets the DC SDK makeimg env + puts SH cc / image tools on PATH |
| `build-image.bat [retail\|debug]` | `makeimg` → `release\<type>\NK.bin` (B000FF CE ROM) |
| `wrap-image.ps1 -NkBin .. -Out ..` | `NK.bin` → bootable `0winceos.bin` (DUMPNK + 0x800 Sega header) |
| `unwrap-image.ps1 -In .. -Out ..` | `0winceos.bin` → raw memory image (inspect) |
| `build-kernel.bat [retail\|debug] [file.c]` | EXPERIMENTAL: compile leaked NK/SHX sources |

## The loop
```
build-image.bat retail                                  # -> NK.bin (validated round-trip)
powershell -File wrap-image.ps1 ^
   -NkBin C:\wcedreamcast\release\retail\NK.bin ^
   -Out   C:\wcedreamcast\release\retail\0winceos.bin   # -> bootable image
```
To change what's in the image, edit `C:\wcedreamcast\release\retail\*.bib` / `*.reg`
before `build-image.bat`. To replace the kernel, drop a freshly-built `nknodbg.exe`
over `release\retail\nknodbg.exe` (what `ce.bib` pulls in as `nk.exe`) then rebuild.

See `../docs/03-build-pipeline.md` for the full validated chain and the wrapper format.
