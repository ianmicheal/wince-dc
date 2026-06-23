@echo off
rem ----------------------------------------------------------------------------
rem  build-kernel.bat [retail|debug] [sourcefile.c]
rem  EXPERIMENTAL: compile the leaked CE 3.0 SH-4 kernel (NK) with the SH cc.
rem  Default = "kernel compile smoke": compile NK\KERNEL\SHX\SHFLOAT.C to an obj.
rem  Full NK link is the next milestone (wire NK private headers iteratively).
rem ----------------------------------------------------------------------------
setlocal
call "%~dp0setenv.bat" %1

set NK=%WINCESRC%\PRIVATE\WINCEOS\COREOS\NK
set OUTDIR=%~dp0..\reference\kernel-obj
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem kernel private headers + SHX + CORE, ahead of the OAK/SDK headers from setenv
set INCLUDE=%NK%\INC;%NK%\KERNEL\SHX;%WINCESRC%\PRIVATE\WINCEOS\COREOS\CORE\INC;%INCLUDE%

rem CE SH-4 kernel defines (retail). Adjust as the compile dictates.
set KDEFS=-DSH4=1 -DSHx=1 -DUNDER_CE=300 -D_WIN32_WCE=300 -DUNICODE -D_UNICODE -DKERNEL

set SRC=%~2
if "%SRC%"=="" set SRC=%NK%\KERNEL\SHX\SHFLOAT.C

echo [build-kernel] cc %SRC%
cl.exe /nologo /c /W3 %KDEFS% /Fo"%OUTDIR%\\" "%SRC%"
echo [build-kernel] errorlevel=%errorlevel%
echo [build-kernel] NOTE: first runs WILL hit missing NK headers; that is the
echo [build-kernel] iteration loop (add the right PRIVATE include dirs above).
endlocal
