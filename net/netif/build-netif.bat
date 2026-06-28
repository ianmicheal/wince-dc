@echo off
rem ----------------------------------------------------------------------------
rem  build-netif.bat  -  build the universal microstk link shim as mppp.dll (SH-4).
rem  Drop-in replacement for the SDK mppp.dll: exports InterfaceInitialize (link
rem  adapter; ethernet via BBA or W5500/MACRAW, dial via modem) + the AfdRas* RAS
rem  stubs, at the exact mppp ordinals (netif.def). Links dcspi.dll for W5500 SPI.
rem ----------------------------------------------------------------------------
setlocal
if "%WCEDREAMCASTROOT%"=="" set WCEDREAMCASTROOT=C:\wcedreamcast
set DCSDK=%WCEDREAMCASTROOT%
if "%~1"=="" (set DCBT=debug) else (set DCBT=%~1)
for %%I in ("%~dp0..\..") do set REPO=%%~fI
set SHBIN=%REPO%\vendor\sh-toolchain\bin\I386\SH
set HOSTBIN=%REPO%\vendor\sh-toolchain\bin\I386
set PATH=%SHBIN%;%HOSTBIN%;%PATH%
set INCLUDE=%DCSDK%\inc;%REPO%\drivers\dcspi
set LIB=%DCSDK%\lib\%DCBT%
set OUT=%~dp0..\..\reference\net-obj
if not exist "%OUT%" mkdir "%OUT%"

rem  Build the SPI transport (W5500 backend loads dcspi.dll on demand; it must be a
rem  module in the image, but mppp has NO link-time dependency on it).
echo [netif] building dcspi transport ...
call "%REPO%\drivers\dcspi\build-dcspi.bat" %DCBT%
if not exist "%REPO%\reference\driver-obj\dcspi.dll" (echo [netif] dcspi.dll MISSING - build failed & exit /b 1)

rem  Assemble the G2-bus interrupt lock (SH-4 SR.IMASK mask/restore; no inline asm in
rem  the MS SH compiler). Needs kxshx.h (LEAF_ENTRY/ENTRY_END) on the asm include path.
echo [netif] assembling g2lock.src (SH-4) ...
set ASMINC=%REPO%\vendor\sh-toolchain\ce3-ppc2k\include
shasm.exe -cpu=SH4 -DSH4=1 -DSHx=1 -nologo -I"%ASMINC%" -object="%OUT%\g2lock.obj" "%~dp0g2lock.src"
if not exist "%OUT%\g2lock.obj" (echo [netif] ASSEMBLE FAILED g2lock & exit /b 1)

set CF=/nologo /c /W3 -DUNDER_CE=212 -D_WIN32_WCE=212 -DUNICODE -D_UNICODE -DSH4=1 -DSHx=1
echo [netif] compiling netif.c + bba_hw.c + ras.c + w5500.c (SH-4) ...
"%SHBIN%\cl.exe" %CF% /Fo"%OUT%\netif.obj"  "%~dp0netif.c"
if errorlevel 1 (echo [netif] COMPILE FAILED netif & exit /b 1)
"%SHBIN%\cl.exe" %CF% /Fo"%OUT%\bba_hw.obj" "%~dp0bba_hw.c"
if errorlevel 1 (echo [netif] COMPILE FAILED bba_hw & exit /b 1)
"%SHBIN%\cl.exe" %CF% /Fo"%OUT%\ras.obj"    "%~dp0ras.c"
if errorlevel 1 (echo [netif] COMPILE FAILED ras & exit /b 1)
"%SHBIN%\cl.exe" %CF% /Fo"%OUT%\w5500.obj"  "%~dp0w5500.c"
if errorlevel 1 (echo [netif] COMPILE FAILED w5500 & exit /b 1)
"%SHBIN%\cl.exe" %CF% /Fo"%OUT%\flashrom.obj" "%~dp0flashrom.c"
if errorlevel 1 (echo [netif] COMPILE FAILED flashrom & exit /b 1)
"%SHBIN%\cl.exe" %CF% /Fo"%OUT%\fblog.obj" "%~dp0fblog.c"
if errorlevel 1 (echo [netif] COMPILE FAILED fblog & exit /b 1)

echo [netif] linking mppp.dll ...
"%HOSTBIN%\link.exe" /nologo /dll /machine:SH4 /subsystem:windowsce,2.12 /entry:dllentry ^
  /def:"%~dp0netif.def" /out:"%OUT%\mppp.dll" ^
  "%OUT%\netif.obj" "%OUT%\bba_hw.obj" "%OUT%\ras.obj" "%OUT%\w5500.obj" "%OUT%\flashrom.obj" "%OUT%\fblog.obj" "%OUT%\g2lock.obj" ^
  "%DCSDK%\lib\%DCBT%\coredll.lib" "%DCSDK%\lib\%DCBT%\corelibc.lib" > "%OUT%\netif.link.log" 2>&1
type "%OUT%\netif.link.log"
echo [netif] errorlevel=%errorlevel%  (out: %OUT%\mppp.dll)
endlocal
