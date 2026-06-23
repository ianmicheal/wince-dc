# dc-bsp — Dreamcast BSP for Windows CE 3.0

The hand-written platform support for booting CE 3.0 on the Dreamcast.
Cloned into `C:\WINCE300\PLATFORM\Dreamcast` at build time. See ../docs/.

oal/dreamcast/  startup.s, OEMInit, TMU tick, INTC/Holly ISR, SCIF debug, KITL
drivers/        PVR2 display, Maple HID, AICA wavedev, GD-ROM block, BBA NDIS
inc/            DC register defs (dreamcast.h, holly.h, ...)
files/          platform.bib / .reg / .dat (image contents)
cesysgen/       which OS modules to sysgen into the image (shell, IE, ...)

Bring-up order: SCIF print -> TMU tick -> MMU/cache -> KITL -> PVR fb -> Maple -> GD-ROM -> shell.
