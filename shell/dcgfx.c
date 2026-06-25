//
// dcgfx.c - DirectDraw + GDI graphics layer for the Dreamcast CE shell.
//
// The DC primary surface is volatile (content not retained when idle), so we draw
// every frame into a persistent OFFSCREEN back buffer (created with an explicit
// 565 format that matches the primary, so COLORFILL colors are correct) and Blt
// it to the primary each loop iteration via GfxPresent.
//
#include "dcgfx.h"
#include <ddraw.h>

static HWND                s_hwnd    = NULL;
static LPDIRECTDRAW        s_dd      = NULL;
static LPDIRECTDRAWSURFACE s_primary = NULL;
static LPDIRECTDRAWSURFACE s_back    = NULL;   // persistent 565 back buffer

HFONT g_FontUI    = NULL;
HFONT g_FontBold  = NULL;
HFONT g_FontTitle = NULL;

static LPDIRECTDRAWSURFACE Target(void)
{
    return s_back ? s_back : s_primary;
}

static WORD ToRgb565(COLORREF c)
{
    return (WORD)(((GetRValue(c) >> 3) << 11) | ((GetGValue(c) >> 2) << 5) | (GetBValue(c) >> 3));
}

static HFONT MakeFont(int height, int weight)
{
    LOGFONTW lf;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    lstrcpyW(lf.lfFaceName, L"Arial");
    return CreateFontIndirectW(&lf);
}

static BOOL CreateSurfaces(void)
{
    DDSURFACEDESC ddsd;
    HRESULT       hr = E_FAIL;
    int           tries;

    for (tries = 0; tries < 30; tries++)
    {
        hr = DirectDrawCreate(NULL, &s_dd, NULL);
        if (hr == DD_OK)
            break;
        s_dd = NULL;
        Sleep(100);
    }
    if (hr != DD_OK)
        return FALSE;

    IDirectDraw_SetCooperativeLevel(s_dd, s_hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    IDirectDraw_SetDisplayMode(s_dd, SCREEN_W, SCREEN_H, 16);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize         = sizeof(ddsd);
    ddsd.dwFlags        = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = IDirectDraw_CreateSurface(s_dd, &ddsd, &s_primary, NULL);
    if (hr != DD_OK)
        return FALSE;

    // persistent back buffer, explicit RGB565 to match the primary
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize         = sizeof(ddsd);
    ddsd.dwFlags        = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;  // sysmem: never "lost"
    ddsd.dwWidth        = SCREEN_W;
    ddsd.dwHeight       = SCREEN_H;
    ddsd.ddpfPixelFormat.dwSize        = sizeof(DDPIXELFORMAT);
    ddsd.ddpfPixelFormat.dwFlags       = DDPF_RGB;
    ddsd.ddpfPixelFormat.dwRGBBitCount = 16;
    ddsd.ddpfPixelFormat.dwRBitMask    = 0xF800;
    ddsd.ddpfPixelFormat.dwGBitMask    = 0x07E0;
    ddsd.ddpfPixelFormat.dwBBitMask    = 0x001F;
    if (IDirectDraw_CreateSurface(s_dd, &ddsd, &s_back, NULL) != DD_OK)
        s_back = NULL;   // fall back to drawing directly on the primary
    {
        WCHAR b[64];
        wsprintfW(b, L"GfxInit: primary=%x back=%x\r\n", (unsigned)s_primary, (unsigned)s_back);
        OutputDebugStringW(b);
    }
    return TRUE;
}

static void DestroySurfaces(void)
{
    if (s_back)    { IDirectDrawSurface_Release(s_back);    s_back = NULL; }
    if (s_primary) { IDirectDrawSurface_Release(s_primary); s_primary = NULL; }
    if (s_dd)      { IDirectDraw_RestoreDisplayMode(s_dd);  IDirectDraw_Release(s_dd); s_dd = NULL; }
}

BOOL GfxInit(HWND hwnd)
{
    s_hwnd = hwnd;
    if (!CreateSurfaces())
        return FALSE;
    g_FontUI    = MakeFont(12, FW_NORMAL);
    g_FontBold  = MakeFont(12, FW_BOLD);
    g_FontTitle = MakeFont(14, FW_BOLD);
    return TRUE;
}

void GfxShutdown(void)
{
    DestroySurfaces();
}

void GfxFill(int left, int top, int right, int bottom, COLORREF color)
{
    DDBLTFX fx;
    RECT    rc;
    LPDIRECTDRAWSURFACE t = Target();

    if (!t)
        return;
    SetRect(&rc, left, top, right, bottom);
    memset(&fx, 0, sizeof(fx));
    fx.dwSize      = sizeof(fx);
    fx.dwFillColor = ToRgb565(color);
    IDirectDrawSurface_Blt(t, &rc, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

void GfxBevel(const RECT *rc, BOOL raised)
{
    COLORREF tl = raised ? RGB(255, 255, 255) : RGB(128, 128, 128);
    COLORREF br = raised ? RGB(128, 128, 128) : RGB(255, 255, 255);

    GfxFill(rc->left,      rc->top,        rc->right,    rc->top + 1, tl);  // top
    GfxFill(rc->left,      rc->top,        rc->left + 1, rc->bottom,  tl);  // left
    GfxFill(rc->left,      rc->bottom - 1, rc->right,    rc->bottom,  br);  // bottom
    GfxFill(rc->right - 1, rc->top,        rc->right,    rc->bottom,  br);  // right
}

HDC GfxLockDC(void)
{
    HDC                 hdc;
    LPDIRECTDRAWSURFACE t = Target();

    if (!t || IDirectDrawSurface_GetDC(t, &hdc) != DD_OK)
        return NULL;
    return hdc;
}

void GfxUnlockDC(HDC hdc)
{
    LPDIRECTDRAWSURFACE t = Target();
    if (t && hdc)
        IDirectDrawSurface_ReleaseDC(t, hdc);
}

void GfxText(HDC hdc, int x, int y, COLORREF fg, COLORREF bg, HFONT font, const WCHAR *text)
{
    if (font)
        SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkColor(hdc, bg);
    SetBkMode(hdc, OPAQUE);
    ExtTextOutW(hdc, x, y, 0, NULL, text, lstrlenW(text), NULL);
}

void GfxPresent(void)
{
    HRESULT hr;

    if (!s_back || !s_primary)
        return;
    hr = IDirectDrawSurface_Blt(s_primary, NULL, s_back, NULL, DDBLT_WAIT, NULL);
    if (hr == DDERR_SURFACELOST)
    {
        IDirectDrawSurface_Restore(s_primary);   // VRAM primary can be lost; sysmem back can't
        IDirectDrawSurface_Blt(s_primary, NULL, s_back, NULL, DDBLT_WAIT, NULL);
    }
}

void GfxLaunch(const WCHAR *path)
{
    PROCESS_INFORMATION pi;

    DestroySurfaces();   // release the exclusive display so the child can own it
    if (CreateProcessW(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi))
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CreateSurfaces();    // take it back
}
