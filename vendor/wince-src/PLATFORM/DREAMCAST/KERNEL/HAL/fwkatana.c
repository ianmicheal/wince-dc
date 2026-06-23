/*
 * fwkatana.c - Dreamcast (Katana) firmware misc (hal:fwkatana.obj).
 * OEMPowerOff; the SH-4 D-cache line count used by the kernel's _FlushDCache.
 */
#include <windows.h>
#include "dc_hw.h"

extern void OEMWriteDebugString(LPCWSTR psz);

/* SH-4 D-cache line count (16 KB / 32 B = 512). Used by _FlushDCache (shexcept). */
DWORD SH4CacheLines = 512;

void OEMPowerOff(void)
{
    OEMWriteDebugString(L"OEM Power off.\r\n");
    /* TODO: mask interrupts (raise SR.IMASK / set BL) before halting. */
    for (;;)                        /* halt */
        ;
}
