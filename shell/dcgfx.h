//
// dcgfx.h - Graphics layer for the Dreamcast CE shell.
//
// The DC has no GDI desktop primary; the screen is a DirectDraw primary over the
// PowerVR. GWES implements only a GDI subset: text (with fonts), DCs and BitBlt -
// but NOT FillRect/brushes/pens/shapes. So this layer draws fills and 3D bevels
// with DirectDraw COLORFILL Blts (the primary is RGB565) and text with GDI fonts
// via the surface DC. All fills must happen before locking the DC for text.
//
#ifndef DCGFX_H
#define DCGFX_H

// NOTE: dcgfx.c defines CINTERFACE itself before including ddraw/d3d (C-style COM). It is
// deliberately NOT defined here so C++ consumers (iexplore.cpp, which hosts the WebBrowser
// control via C++ COM) can include this header without turning every COM interface C-style.
#include <windows.h>
#include "dcwin.h"     // ICON_* ids (shared with the compositor protocol)

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_W 640
#define SCREEN_H 480

BOOL GfxInit(HWND hwnd);
void GfxShutdown(void);

//
// Fill / bevel - DirectDraw COLORFILL (call these BEFORE GfxLockDC).
//
void GfxFill(int left, int top, int right, int bottom, COLORREF color);
void GfxBevel(const RECT *rc, BOOL raised);

//
// Text - GDI on the surface DC. GfxLockDC must be balanced with GfxUnlockDC, and
// no GfxFill may run while the DC is locked.
//
HDC  GfxLockDC(void);
void GfxUnlockDC(HDC hdc);
void GfxText(HDC hdc, int x, int y, COLORREF fg, COLORREF bg, HFONT font, const WCHAR *text);
int  GfxTextWidth(HFONT font, const WCHAR *text);   // pixel width of text in the given font

//
// Present the composited scene + pointer via the page-flip chain (Blt scene->back
// + cursor + Flip(DDFLIP_WAIT), which is vsync-paced and yields the CPU). Call it
// ONLY when something changed (content dirty or the cursor moved) - flip-chain
// surfaces retain content, so there's no need to present every frame. Returns TRUE
// if a surface was lost (caller must re-render the scene first).
//
BOOL GfxPresent(int cursorX, int cursorY, BOOL showCursor);

//
// 16x16 color-keyed icons (built from embedded art into DDraw surfaces). Blit in
// the fills pass (it's a Blt, not GDI). Ids are shared with the DCOP_ICON command.
//
void GfxIcon(int id, int x, int y);      // 16x16
void GfxIconBig(int id, int x, int y);   // 32x32
void GfxSetDragGhost(int iconId);        // translucent 32x32 icon trailing the cursor (-1 = off)

// Compositor clip rect: subsequent fills/text/icons are clipped to [x0,y0)-(x1,y1).
// Used to clip a window's client content to its (resizable) client area.
void GfxSetClip(int x0, int y0, int x1, int y1);
void GfxClearClip(void);

//
// Launch an app: hand the exclusive display off, wait for it, reclaim it.
//
void GfxLaunch(const WCHAR *path);

//
// Static desktop-layer cache. Paint the desktop once between Begin/End (into a
// private buffer); GfxBlitDesktopCache() then stamps it into the frame buffer per
// recomposite instead of repainting fills+icons+labels every frame.
//
void GfxBeginDesktopCache(void);
void GfxEndDesktopCache(void);
void GfxBlitDesktopCache(void);

// Block until the PVR vblank (~60Hz) - pace the loop without Sleep's 50ms-tick rounding.
HRESULT GfxWaitVBlank(void);

//
// Page layer (for the browser): the Trident WebBrowser control renders via GWES into the
// GDI framebuffer, which the PVR does NOT scan out while we hold the DDraw flip primary. So
// we wrap that framebuffer (GETGDIINFO + MmMapIoSpace) and each frame BltFast the requested
// region into a VRAM texture, drawn as a normal compositor quad. GfxInitPageLayer once after
// GfxInit; GfxBlitPage(src rect in screen/GDI coords -> dst rect on our scene) per frame.
//
BOOL GfxInitPageLayer(void);
void GfxBlitPage(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh);

extern HFONT g_FontUI;
extern HFONT g_FontBold;
extern HFONT g_FontTitle;

#ifdef __cplusplus
}
#endif

#endif // DCGFX_H
