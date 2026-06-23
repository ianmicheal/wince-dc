/*
 * ktimer.c - Dreamcast SH-4 TMU bring-up + tick (hal:ktimer.obj).
 *
 * RECONSTRUCTED: InitClock programs TMU0 = periodic system tick (25 ms/underflow),
 * TMU1 = aux, TMU2 = free-running down-counter for QueryPerformanceCounter.
 * SC_GetTickCount returns the kernel CurMSec. The timer ISRs are in timer.c.
 */
#include <windows.h>
#include "dc_hw.h"

extern BOOL HookInterrupt(int idInt, FARPROC pfn);
extern void Timer0ISR(void);
extern void Timer1ISR(void);
extern volatile DWORD CurMSec;          /* kernel GetTickCount base */

/* OAL clock globals + the reschedule accumulator timer.c advances. */
DWORD g_TicksPerPeriod;          /* TMU0 reload = 312500 -> 25 ms at PCLK/4 */
DWORD g_TicksTo64kMSec;          /* = 5 (scaling constant from the shipped OAL) */
volatile DWORD dwReschedTime;

void InitClock(void)
{
    g_TicksTo64kMSec = 5;
    g_TicksPerPeriod = 312500;

    /* --- TMU0: periodic system tick --- */
    VUINT8(SH4_TMU_TSTR)  &= (BYTE)~SH4_TSTR_STR0;       /* stop TMU0 */
    VUINT16(SH4_TMU_TCR0)  = SH4_TMU_TCR_UNIE;           /* 0x20: underflow int enable */
    VUINT32(SH4_TMU_TCOR0) = g_TicksPerPeriod;
    VUINT32(SH4_TMU_TCNT0) = g_TicksPerPeriod;
    VUINT16(SH4_INTC_IPRA) = (VUINT16(SH4_INTC_IPRA) & 0x0FFF) | 0x1000;  /* TMU0 prio */
    HookInterrupt(INTRVEC_TMU0, (FARPROC)Timer0ISR);

    /* --- TMU1: aux timer --- */
    VUINT8(SH4_TMU_TSTR)  &= (BYTE)~SH4_TSTR_STR1;       /* stop TMU1 */
    VUINT16(SH4_TMU_TCR1)  = 0;
    VUINT32(SH4_TMU_TCOR1) = 125000;
    VUINT32(SH4_TMU_TCNT1) = 125000;
    VUINT16(SH4_INTC_IPRA) = (VUINT16(SH4_INTC_IPRA) & 0xF0FF) | 0x0100;  /* TMU1 prio */
    HookInterrupt(INTRVEC_TMU1, (FARPROC)Timer1ISR);

    /* --- TMU2: free-running counter for QueryPerformanceCounter --- */
    VUINT8(SH4_TMU_TSTR)  &= (BYTE)~SH4_TSTR_STR2;       /* stop TMU2 */
    VUINT16(SH4_TMU_TCR2)  = 0x18;
    VUINT32(SH4_TMU_TCOR2) = 0xFFFFFFFF;

    /* start TMU0 + TMU2 */
    VUINT8(SH4_TMU_TSTR)  |= (SH4_TSTR_STR0 | SH4_TSTR_STR2);
}

/* GetTickCount backing - the leak has no SC_GetTickCount body; return CurMSec. */
DWORD SC_GetTickCount(void)
{
    return CurMSec;
}
