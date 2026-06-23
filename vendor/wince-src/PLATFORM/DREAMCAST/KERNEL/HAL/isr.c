/*
 * isr.c - Dreamcast (Katana) interrupt service routines.
 *
 * RECONSTRUCTED from hal:isr.obj: the three Holly IRL demuxers KatanaISR2/4/6,
 * the per-source DMAC0-3ISR/JTAGISR/SCIFISR. The SYSINTR dispatch + GInterruptList
 * are in cfwkatan.c. See OAL-NOTES.md.
 *
 * Each KatanaISRn reads (SB_IST<class> & SB_IML<level><class>), masks the top
 * pending source in SB_IML (mask-on-receipt; OEMInterruptDone re-enables), and
 * returns the CE SYSINTR. IRL6=EXT (Maple/GD/AICA/BBA), IRL4=NRM (PVR/TA/DMA),
 * IRL2=ERR.
 */
#include <windows.h>
#include "dc_hw.h"

#define SYSINTR_NOP 0

/* IRL6 / external sources -> SYSINTR 0x14/0x17/0x1A/0x1B (status bits 0..3). */
ULONG KatanaISR6(void)
{
    DWORD pending = VUINT32(SB_ISTEXT) & VUINT32(SB_IML6EXT);
    if (pending & 0x1) { VUINT32(SB_IML6EXT) &= 0xFFFFFFFE; return 0x14; }
    if (pending & 0x2) { VUINT32(SB_IML6EXT) &= 0xFFFFFFFD; return 0x17; }
    if (pending & 0x4) { VUINT32(SB_IML6EXT) &= 0xFFFFFFFB; return 0x1A; }
    if (pending & 0x8) { VUINT32(SB_IML6EXT) &= 0xFFFFFFF7; return 0x1B; }
    return SYSINTR_NOP;
}

/* IRL4 / normal sources -> SYSINTR 0x10/0x12/0x15/0x18. */
ULONG KatanaISR4(void)
{
    DWORD pending = VUINT32(SB_ISTNRM) & VUINT32(SB_IML4NRM);
    if (pending & 0x00000FFF) { VUINT32(SB_IML4NRM) &= 0xFFC7F000; return 0x10; } /* bits 0-11 */
    if (pending & 0x00003000) { VUINT32(SB_IML4NRM) &= 0xFFFFCFFF; return 0x12; } /* bits 12-13 */
    if (pending & 0x00004000) { VUINT32(SB_IML4NRM) &= 0xFFFFBFFF; return 0x15; } /* bit 14 */
    if (pending & 0x00008000) { VUINT32(SB_IML4NRM) &= 0xFFFF7FFF; return 0x18; } /* bit 15 */
    return SYSINTR_NOP;
}

/* IRL2 / error sources -> SYSINTR 0x11/0x13/0x16/0x19. */
ULONG KatanaISR2(void)
{
    DWORD pending = VUINT32(SB_ISTERR) & VUINT32(SB_IML2ERR);
    if (pending & 0x000000FF) { VUINT32(SB_IML2ERR) &= 0xEFFFFF00; return 0x11; } /* bits 0-7 */
    if (pending & 0x00000F00) { VUINT32(SB_IML2ERR) &= 0xFFFFF0FF; return 0x13; } /* bits 8-11 */
    if (pending & 0x00007000) { VUINT32(SB_IML2ERR) &= 0xFFFF8FFF; return 0x16; } /* bits 12-14 */
    if (pending)              { VUINT32(SB_IML2ERR) &= 0xFFFF7FFF; return 0x19; }
    return SYSINTR_NOP;
}

/* Per-source ISR stubs hooked by OEMInit. No drivers attached during bring-up,
 * so these acknowledge nothing and request no reschedule. */
ULONG DMAC0ISR(void) { return SYSINTR_NOP; }
ULONG DMAC1ISR(void) { return SYSINTR_NOP; }
ULONG DMAC2ISR(void) { return SYSINTR_NOP; }
ULONG DMAC3ISR(void) { return SYSINTR_NOP; }
ULONG JTAGISR(void)  { return SYSINTR_NOP; }
ULONG SCIFISR(void)  { return SYSINTR_NOP; }   /* debug I/O is polled, not IRQ */
