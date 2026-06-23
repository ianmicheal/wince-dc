# reference/ — build artifacts & provenance

Binaries here are **gitignored** (large + copyrighted MS/Sega content, reproducible
from the SDK). This manifest records what they are and their SHA-256 so a rebuild can
be checked for structural fidelity.

| file | bytes | SHA-256 | what |
|------|-------|---------|------|
| `0winceos.retail.orig.bin` | 1,888,256 | `C5C0D53F…54A37758` | **Shipped reference** — Sega's retail kernel image, untouched. |
| `0winceos.rebuilt.bin`     | 1,888,256 | `B11CFB1F…F703C783` | Ours: `makeimg`→`NK.bin`→`wrap-image.ps1`. **Header 0x00–0x2F byte-identical to shipped; same total size.** Payload differs (rebuilt module timestamps) — expected. |
| `NK.retail.rebuilt.bin`        | 1,842,003 | `FE08C1FA…50A3B917` | `makeimg` output — canonical **B000FF** CE ROM (29 modules). |
| `NK.retail.rebuilt.rawmem.bin` | 1,885,244 | `46DAB36C…BA4E155D` | `DUMPNK NK.bin` — flat raw memory image (loads @ 0x8C010000). |
| `makeimg.retail.rebuilt.out`   | text | — | makeimg build log (committed). |

## Validation summary (2026-06-23)
- `makeimg` round-trips the shipped image: 29 modules, start `8C010000`, ROM span `0x1CC43C`.
- `DUMPNK` parses our `NK.bin` (rejects the wrapped shipped image — it predates the wrapper).
- `wrap-image.ps1` reconstructs the 0x800 Sega header **byte-for-byte** (base `0x0C010000`,
  payload off `0x800`, len `0x1CC800`) and the exact 1,888,256-byte total.

Reproduce: `toolchain\build-image.bat retail` then `toolchain\wrap-image.ps1`.
