//
// fblog.c - on-screen POST trail for the link shim, for debugging on REAL hardware
// where there's no serial console (nknodbg) and the framebuffer is all we have.
//
// FbLog(rgb) paints the next full-width stripe down the screen AND sets the PVR border
// colour. Call it at each probe/init stage: if the shim hangs/faults on a given board,
// the stripes stop at that stage's colour, localising the crash (e.g. SCI SpiInit vs the
// W5500 VERSIONR read vs BBA init). Reads the live framebuffer offset from PVR_FB_ADDR so
// it draws to whatever CE is displaying; all under SetKMode(TRUE) + __try so a bad write
// can never crash the thing we're trying to diagnose.
//
// Gated by HKLM\Comm\Netif "FbLog"=dword:1 (read once) so it's a no-op in normal builds.
//
#include <windows.h>

DWORD SetKMode(DWORD fMode);

#define PVR_BORDER   (*(volatile DWORD *)0xA05F8040)   // border colour (RGB888)
#define PVR_FB_ADDR  (*(volatile DWORD *)0xA05F8050)   // display start offset in VRAM
#define VRAM_BASE    0xA5000000u                       // VRAM, CPU 32-bit P2 area
#define VRAM_MASK    0x007FFFFFu                        // 8 MB
#define FB_W         640
#define FB_H         480

static int s_slot;
static int s_on = -1;                                   // -1 = not yet read from registry

static int fblog_enabled(void)
{
    HKEY  h;
    DWORD v = 0, t, n = sizeof(v);
    if (s_on >= 0) return s_on;
    s_on = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Comm\\Netif", 0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(h, L"FbLog", 0, &t, (BYTE *)&v, &n) == ERROR_SUCCESS && v) s_on = 1;
        RegCloseKey(h);
    }
    return s_on;
}

static WORD to565(DWORD rgb)   // rgb = 0xRRGGBB -> RGB565
{ return (WORD)((((rgb >> 19) & 0x1F) << 11) | (((rgb >> 10) & 0x3F) << 5) | ((rgb >> 3) & 0x1F)); }

void FbLog(DWORD rgb)
{
    DWORD prev;
    if (!fblog_enabled()) return;
    prev = SetKMode(TRUE);
    __try
    {
        DWORD off = PVR_FB_ADDR & VRAM_MASK;
        volatile WORD *fb = (volatile WORD *)(VRAM_BASE + off);
        WORD  c  = to565(rgb);
        int   y0 = s_slot * 8, y, x;
        if (y0 + 6 <= FB_H)
            for (y = y0; y < y0 + 6; y++)
                for (x = 0; x < FB_W; x++)
                    fb[y * FB_W + x] = c;
        PVR_BORDER = rgb & 0x00FFFFFF;                  // also a persistent POST code
        s_slot++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    SetKMode(prev);
}
