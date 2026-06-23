/*
 * compress.c - ROM compression stubs (kernel compress.obj; absent from the leak).
 * With COMPRESSION off in the image these aren't hit; stub as "no compression".
 */
#include <windows.h>

DWORD CECompress(LPBYTE bufIn, DWORD cbIn, LPBYTE bufOut, DWORD cbOut,
                 WORD step, DWORD pagesize)
{
    return 0;       /* 0 bytes produced => caller stores uncompressed */
}

DWORD CEDecompress(LPBYTE bufIn, DWORD cbIn, LPBYTE bufOut, DWORD cbOut,
                   DWORD skip, WORD step, DWORD pagesize)
{
    return (DWORD)-1;
}
