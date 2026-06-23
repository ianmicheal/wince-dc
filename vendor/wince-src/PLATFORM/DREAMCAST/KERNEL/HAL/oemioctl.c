/*
 * oemioctl.c - Dreamcast HAL IOCTL dispatch (hal:oemioctl.obj).
 * The shipped OEMIoControl dispatches HAL IOCTLs (WDM init, interrupt
 * status/include/exclude, platform version, SetRTC). Minimal for bring-up:
 * answer nothing (FALSE). See OAL-NOTES.md.
 */
#include <windows.h>
#include "dc_hw.h"

BOOL OEMIoControl(DWORD dwIoControlCode, LPVOID pInBuf, DWORD nInBufSize,
                  LPVOID pOutBuf, DWORD nOutBufSize, LPDWORD pBytesReturned)
{
    /* TODO: handle IOCTL_HAL_REQUEST_SYSINTR / INTR status+include+exclude,
     * IOCTL_HAL_GET_DEVICEID, reboot, RTC. */
    if (pBytesReturned)
        *pBytesReturned = 0;
    return FALSE;
}
