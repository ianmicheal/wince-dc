/*
 * mdppfs.c - Dreamcast parallel-port file system / ASE debug-adapter link
 *            (hal:mdppfs.obj). The shipped kernel's debug I/O went over the Sega
 *            Debug Adapter via this path; we drive debug over the SCIF (debug.c)
 *            instead, so these are stubbed and NoPPFS=1.
 */
#include <windows.h>
#include "dc_hw.h"

/* 1 => no parallel-port file system / debug link present (we use the SCIF). */
DWORD NoPPFS = 1;

int  OEMParallelPortInit(void)            { NoPPFS = 1; return 1; }
int  OEMParallelPortGetByte(void)         { return -1; }
void OEMParallelPortSendByte(BYTE ch)     { }

/* Debug comm error clear (KITL/debugger path) - unused. */
void OEMClearDebugCommError(void)         { }
