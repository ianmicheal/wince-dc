/*
 * rtc.c - Dreamcast RTC read (hal:rtc.obj).
 * The shipped OEMGetRealTime reads the AICA RTC (0xA0710000/04 = seconds since
 * 1950). For bring-up we return a fixed time; wiring the real RTC is a TODO.
 * OEMSetRealTime is in oemwdm.c (matching the PDB); OEMSetAlarmTime returns 0.
 */
#include <windows.h>
#include "dc_hw.h"

/* DC AICA RTC: 32-bit seconds since 1950-01-01, split across two regs. */
#define DC_RTC_HI   0xA0710000
#define DC_RTC_LO   0xA0710004

int OEMGetRealTime(LPSYSTEMTIME pst)
{
    /* TODO: read DC_RTC_HI/LO and convert to SYSTEMTIME. */
    if (pst) {
        pst->wYear = 2000; pst->wMonth = 1; pst->wDayOfWeek = 6; pst->wDay = 1;
        pst->wHour = 0; pst->wMinute = 0; pst->wSecond = 0; pst->wMilliseconds = 0;
    }
    return 1;
}

int OEMSetAlarmTime(LPSYSTEMTIME pst)  { return 0; }
