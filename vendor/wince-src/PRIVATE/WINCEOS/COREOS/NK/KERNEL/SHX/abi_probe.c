/* ABI size/offset probe — NOT a build member. Emits sizeof/offsetof via the
   compiler's "cannot convert from 'int' to 'char (*)[N]'" error (C2440),
   so we read layout without running an SH-4 binary. Compile-only. */
#include <windows.h>
#include <kernel.h>

#define EMIT(tag,val)  char (*tag)[ (val) ] = 1;

EMIT(SZ_Process,   sizeof(struct Process))
EMIT(SZ_Thread,    sizeof(struct Thread))
EMIT(SZ_KData,     sizeof(struct KDataStruct))
EMIT(OFF_Proc_pTh,        offsetof(struct Process, pTh))
EMIT(OFF_Proc_aky,        offsetof(struct Process, aky))
EMIT(OFF_Thr_pProc,       offsetof(struct Thread, pProc))
EMIT(OFF_Thr_pOwnerProc,  offsetof(struct Thread, pOwnerProc))
EMIT(OFF_Thr_tlsPtr,      offsetof(struct Thread, tlsPtr))
EMIT(OFF_KData_pCurThd,   offsetof(struct KDataStruct, pCurThd))
EMIT(OFF_KData_handleBase,offsetof(struct KDataStruct, handleBase))
EMIT(OFF_KData_aInfo,     offsetof(struct KDataStruct, aInfo))
EMIT(OFF_Thr_ctx,   offsetof(struct Thread, ctx))
EMIT(SZ_CPUCONTEXT, sizeof(CPUCONTEXT))
EMIT(OFF_Proc_oe,   offsetof(struct Process, oe))
EMIT(OFF_Proc_e32,  offsetof(struct Process, e32))
EMIT(SZ_e32_lite,   sizeof(e32_lite))
EMIT(SZ_o32_lite,   sizeof(o32_lite))
EMIT(SZ_openexe_t,  sizeof(openexe_t))
EMIT(SZ_cinfo,      sizeof(struct cinfo))
EMIT(SZ_HDATA,      sizeof(struct _HDATA))
