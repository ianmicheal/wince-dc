/*
 * fwinit.c - Dreamcast firmware-init C side (hal:fwinit.obj).
 * The boot entry StartUp (+ OEMNMI, the Flycast MMU magic) is the asm in
 * fwinit.src; OEMIdle is the kernel idle hook.
 */
#include <windows.h>
#include "dc_hw.h"

/* Kernel idle hook. A real OAL would sleep the CPU (SH-4 'sleep') until the next
 * interrupt; for bring-up just return so the scheduler spins. */
void OEMIdle(DWORD dwIdleParam)
{
    /* TODO: lower power via 'sleep' once the tick/wake path is trusted. */
}
