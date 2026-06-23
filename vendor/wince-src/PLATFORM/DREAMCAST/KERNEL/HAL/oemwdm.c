/*
 * oemwdm.c - Dreamcast WDM/bus glue (hal:oemwdm.obj).
 * OEMGetExtensionDRAM (no extension DRAM on the DC), OEMSetRealTime.
 * (Bus access OEMGetBusDataByOffset/OEMSetBusDataByOffset are TODO.)
 */
#include <windows.h>
#include "dc_hw.h"

/* No extension DRAM on the DC (16 MB main only). Shipped kernel: return 0. */
int OEMGetExtensionDRAM(ULONG *pStart, ULONG *pLen)
{
    return 0;
}

int OEMSetRealTime(LPSYSTEMTIME pst)   { return 0; }
