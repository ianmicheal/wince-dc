@echo off
rem ============================================================================
rem  probe-abi.bat [retail|debug]
rem  Compile-only ABI probe: emits sizeof/offsetof of Process/Thread/KDataStruct
rem  (from the leak KERNEL.H) via the C2440 "char (*)[N]" error trick, using the
rem  EXACT build-nklib.bat include chain + kernel defines. Compare vs the SDK
rem  PDB (cvdump): Process=156, Thread=324, KDataStruct=896.
rem ============================================================================
setlocal enabledelayedexpansion
call "%~dp0setenv.bat" %1

set NK=%WINCESRC%\PRIVATE\WINCEOS\COREOS\NK
set KERNEL=%NK%\KERNEL
set SHX=%KERNEL%\SHX
set OUTDIR=%~dp0..\reference\kernel-obj
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set CE3SDK=%GWESLAB%\ce3-ppc2k\include
set BSPINC=%~dp0..\bsp\inc
set INCLUDE=%BSPINC%;%NK%\INC;%SHX%;%NK%\..\CORE\INC;%GWESLAB%\ce3-oak\INC;%CE3SDK%
set KDEFS=-DSH4=1 -DSHx=1 -DUNDER_CE=300 -D_WIN32_WCE=300 -DUNICODE -D_UNICODE -DKERNEL -DWINCEOEM=1 -DWINCEMACRO -DIN_KERNEL -DDBGSUPPORT
if /I "%BLDTYPE%"=="debug" set KDEFS=%KDEFS% -DDEBUG

cl.exe /nologo /c /W3 %KDEFS% /Fo"%OUTDIR%\abi_probe.obj" "%SHX%\abi_probe.c" 2>&1
endlocal
