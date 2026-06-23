/*
 * kdstub.c - kernel JIT + debugger stubs missing from the WINCEOS leak snapshot
 *            (ModuleJit=loader.obj, InitializeJit=kmisc.obj, PKDInit=kwin32.obj).
 */
#include <windows.h>

/* JIT loader (CE 3.0). ModuleJit is also in the syscall table; InitializeJit
 * (PFNOPEN,PFNCLOSE) returns 0 => no JIT present. */
DWORD ModuleJit(LPCWSTR p1, LPWSTR p2, HANDLE *p3)  { return 0; }
int   InitializeJit(void *pfnOpen, void *pfnClose)  { return 0; }

/* PKDInit is a function-POINTER the kernel null-checks (KernelInit2:
 * `if (PKDInit) PKDInit(...)`). MUST be NULL for a no-debugger kernel - a stub
 * FUNCTION makes the check non-null and the kernel calls garbage. */
BOOLEAN (*PKDInit)(LPVOID *, LPVOID *, LPVOID *, LPVOID, LPVOID *, LPVOID *) = NULL;
