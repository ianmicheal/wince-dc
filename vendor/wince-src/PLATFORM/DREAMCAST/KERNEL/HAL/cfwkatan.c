/*
 * cfwkatan.c - Dreamcast (Katana) core HAL framework.
 *
 * RECONSTRUCTED from the shipped SDK kernel (hal:cfwkatan.obj): OEMInit, the
 * OEMInterrupt{Enable,Disable,Done} SYSINTR dispatch, the GInterruptList table,
 * platform-revision detect, and SerialInit. The Holly IRL demuxers and per-source
 * ISRs live in isr.c; the TMU in ktimer.c; see OAL-NOTES.md.
 */
#include <windows.h>
#include "dc_hw.h"

#define SYSINTR_FIRMWARE    0x10
#define NUM_SYSINTR         0x13        /* 19 dispatch slots */

extern BOOL  HookInterrupt(int idInt, FARPROC pfn);
extern DWORD pTOC;                      /* ROMHDR* */
extern void  InitClock(void);
extern void  OEMParallelPortInit(void);
extern void  SerialInit(void);

/* per-source ISRs hooked into the INTC (isr.c) */
extern void KatanaISR2(void), KatanaISR4(void), KatanaISR6(void);
extern void JTAGISR(void);
extern void DMAC0ISR(void), DMAC1ISR(void), DMAC2ISR(void), DMAC3ISR(void);
extern void SCIFISR(void);

/* ---- platform revision: Katana "Set 4" (retail-ish) / "Set 5" (dev HW) ------*/
DWORD OEMPlatformVersion = 4;

int OEMGetPlatformVersion(void *pv)
{
    if (pv) {
        ((DWORD *)pv)[0] = OEMPlatformVersion;   /* Set */
        ((DWORD *)pv)[1] = 0;                     /* Revision */
    }
    return 1;
}

/* ---- SYSINTR dispatch table -------------------------------------------------*/
typedef void (*PFN_INTR)(int sysintr);
typedef struct _INTR_VECTOR {
    PFN_INTR pfnEnable;
    PFN_INTR pfnDisable;
    PFN_INTR pfnDone;
} INTR_VECTOR;

/* Zeroed at boot; per-source drivers (Maple/PVR/GD/AICA/BBA) fill the thunks. */
INTR_VECTOR GInterruptList[NUM_SYSINTR];

BOOL OEMInterruptEnable(DWORD idInt, LPVOID pvData, DWORD cbData)
{
    DWORD idx = idInt - SYSINTR_FIRMWARE;
    if (idx < NUM_SYSINTR) {
        if (GInterruptList[idx].pfnEnable)
            GInterruptList[idx].pfnEnable(idx);
        return TRUE;
    }
    return FALSE;
}

void OEMInterruptDisable(DWORD idInt)
{
    DWORD idx = idInt - SYSINTR_FIRMWARE;
    if (idx < NUM_SYSINTR && GInterruptList[idx].pfnDisable)
        GInterruptList[idx].pfnDisable(idx);
}

void OEMInterruptDone(DWORD idInt)
{
    DWORD idx = idInt - SYSINTR_FIRMWARE;
    if (idx < NUM_SYSINTR && GInterruptList[idx].pfnDone)
        GInterruptList[idx].pfnDone(idx);
}

/* ---- SerialInit: hook the four SCIF interrupt sources (ERI/RXI/BRI/TXI) -----*/
void SerialInit(void)
{
    HookInterrupt(INTRVEC_SCIF + 0, (FARPROC)SCIFISR);   /* ERI */
    HookInterrupt(INTRVEC_SCIF + 1, (FARPROC)SCIFISR);   /* RXI */
    HookInterrupt(INTRVEC_SCIF + 2, (FARPROC)SCIFISR);   /* BRI */
    HookInterrupt(INTRVEC_SCIF + 3, (FARPROC)SCIFISR);   /* TXI */
}

/* ---- OEMInit: the master boot bring-up ------------------------------------*/
static const WCHAR szSet5[] = L"Set 5 is detected.\r\n";
static const WCHAR szSet4[] = L"Set 4 is detected.\r\n";

void OEMInit(void)
{
    DWORD *pRamFixup;

    /* 1. Mask every interrupt source: SH-4 INTC priorities + all 9 Holly masks. */
    VUINT16(SH4_INTC_IPRA) = 0;
    VUINT16(SH4_INTC_IPRB) = 0;
    VUINT16(SH4_INTC_IPRC) = 0;
    VUINT32(SB_IML2NRM) = 0; VUINT32(SB_IML2EXT) = 0; VUINT32(SB_IML2ERR) = 0;
    VUINT32(SB_IML4NRM) = 0; VUINT32(SB_IML4EXT) = 0; VUINT32(SB_IML4ERR) = 0;
    VUINT32(SB_IML6NRM) = 0; VUINT32(SB_IML6EXT) = 0; VUINT32(SB_IML6ERR) = 0;

    /* 2. Detect Katana board revision. */
    OEMGetPlatformVersion(NULL);

    /* 3-4. Bring up the TMU tick and the debug parallel port. */
    InitClock();
    OEMParallelPortInit();

    if (OEMPlatformVersion == 5)
        NKDbgPrintfW(szSet5);
    else if (OEMPlatformVersion == 4)
        NKDbgPrintfW(szSet4);

    /* 5. INTC vector -> ISR map. */
    HookInterrupt(INTRVEC_HOLLY2, (FARPROC)KatanaISR2);   /* 0x0D : IRL level 2 */
    HookInterrupt(INTRVEC_HOLLY4, (FARPROC)KatanaISR4);   /* 0x0B : IRL level 4 */
    HookInterrupt(INTRVEC_HOLLY6, (FARPROC)KatanaISR6);   /* 0x09 : IRL level 6 */
    HookInterrupt(INTRVEC_JTAG,   (FARPROC)JTAGISR);      /* 0x20 */
    HookInterrupt(INTRVEC_DMAC0+0,(FARPROC)DMAC0ISR);     /* 0x22..0x25 */
    HookInterrupt(INTRVEC_DMAC0+1,(FARPROC)DMAC1ISR);
    HookInterrupt(INTRVEC_DMAC0+2,(FARPROC)DMAC2ISR);
    HookInterrupt(INTRVEC_DMAC0+3,(FARPROC)DMAC3ISR);

    /* 6. SCIF serial interrupts. */
    SerialInit();

    /* 7. Clear a 3-DWORD RAM scratch via the P2 uncached alias (ROMHDR+0x18),
     *    then enable the DMAC. */
    pRamFixup = (DWORD *)((*(DWORD *)((char *)pTOC + 0x18)) | P2_UNCACHED);
    pRamFixup[0] = 0;
    pRamFixup[1] = 0;
    pRamFixup[2] = 0;

    VUINT32(SH4_DMAC_DMAOR) = SH4_DMAOR_INIT;
}
