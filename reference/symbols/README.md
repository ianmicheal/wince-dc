# reference/symbols — kernel symbol + ABI dumps

Extracted from the **shipped Sega DC SDK** kernel `.map` / `.pdb`
(`C:\wcedreamcast\release\{retail,debug}\`). These are the spec for our
from-source kernel and the userland bring-up — the SDK kernel is the reference
implementation we mirror. Regenerate after any SDK change.

## Files
- `*.symbols.txt` — public symbol table per kernel variant, RVA-sorted, demangled
  (`undname.exe`), annotated with the originating `lib:object`. Columns: `RVA  OBJ  SYMBOL`.
  - `nknodbg-retail` — the no-KD retail kernel **we mirror** (864 syms / 65 objs).
  - `nk-retail` / `nkscifkd-retail` — full WinDbg-KD and SCIF-KD kernels (995 / 1000).
  - `nknodbg-debug` — debug build (883).
- `*.by-object.txt` — symbol count per object = the link recipe
  (`nk:*` core, `hal:*` OAL, `fulllibc:*` CRT, `asedbg:*` KD serial, `wdmhal:*`, `nokd.obj`).
- `pdb/nknodbg-retail.types.txt` — `cvdump -t` CodeView **type** records (6219 lines).
- `pdb/nknodbg-retail.cvsym.txt` — `cvdump -s` CodeView **symbol** records (17611 lines).
- `pdb/nknodbg-retail.structs.txt` — **expanded struct layouts** (100 aggregates,
  offset+type+member), parsed from `types.txt`. The ABI reference.

## How regenerated
- maps: `undname.exe` demangle + object grouping (parser kept ad-hoc).
- PDBs are **format 2.00** ("Microsoft C/C++ program database 2.00", VC6/CE-era "JG").
  Modern DIA/`llvm-pdbutil` only read 7.00 — only **`cvdump.exe`** parses 2.00.
  Tool fetched from the public `microsoft/microsoft-pdb` repo into
  `vendor/sh-toolchain/bin/I386/cvdump.exe` (**gitignored — Microsoft binary, not vendored**).
  ```
  cvdump -t nknodbg.pdb   # types  -> structs/ABI
  cvdump -s nknodbg.pdb   # symbols (globals, statics, with type ids)
  ```

## Key ABI facts (the userland wall)
Authoritative sizes/offsets the stock 2.12 modules vs our 3.0 kernel must agree on:
- `KDataStruct` **896 B** — `lpvTls`@0, `pCurPrc`@144, `pCurThd`@148, `handleBase`@156,
  `aSections`@160, `aInfo`@768. Confirms the reconstructed `CURTLSPTR_OFFSET=0x000`,
  `KINFO_OFFSET=0x300` (=768).
- `Process` **156 B** — `procnum`@0, `pTh`@16, `aky`@20, `BasePtr`@24, `oe`@72, `e32`@88, `o32_ptr`@152.
- `Thread` **324 B** — `pProc`@12, `pOwnerProc`@16, `aky`@20, `tlsPtr`@36, `ctx`(CONTEXT 228B)@92, `dwThreadTime`@320.
- `_HDATA` **32 B** (handle entry) — `hValue`@8, `ref`@16, `pci`@20, `pvObj`@24.
- `cinfo` **20 B** (PSL API-set call info) — `disp`@4, `type`@5, `cMethods`@6, `ppfnMethods`@8, `pServer`@16.
- `Module` 188, `e32_exe` 248, `e32_lite` 64, `o32_lite` 28, `openexe_t` 16 — PE loader ABI.
