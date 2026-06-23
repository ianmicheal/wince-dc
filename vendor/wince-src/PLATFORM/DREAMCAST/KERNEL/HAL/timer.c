/*
 * timer.c - Dreamcast TMU interrupt handlers (hal:timer.obj).
 * Timer0ISR = system tick (hooked at INTRVEC_TMU0 by InitClock). Timer1ISR = aux.
 */
#include <windows.h>
#include "dc_hw.h"

#ifndef SYSINTR_RESCHED
#define SYSINTR_RESCHED   1
#endif
#define SYSINTR_NOP 0

extern volatile DWORD CurMSec;          /* kernel GetTickCount base */
extern volatile DWORD dwReschedTime;    /* preemption accumulator (ktimer.c) */

/* TMU0 underflow -> system tick. Returns the SYSINTR the kernel should schedule. */
ULONG Timer0ISR(void)
{
    USHORT tcr;

    /* Ack underflow: clear UNF (bit 8) in TCR0, poll until it stays clear. */
    tcr = VUINT16(SH4_TMU_TCR0);
    do {
        VUINT16(SH4_TMU_TCR0) = (USHORT)(tcr & 0x00FF);
        tcr = VUINT16(SH4_TMU_TCR0);
    } while (tcr & 0x0100);

    CurMSec      += 25;          /* GetTickCount base */
    dwReschedTime += 25;         /* preemption accumulator */
    return SYSINTR_RESCHED;
}

ULONG Timer1ISR(void) { return SYSINTR_NOP; }   /* aux timer (TODO) */
