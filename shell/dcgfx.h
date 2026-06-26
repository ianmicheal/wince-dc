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

#define CINTERFACE
#include <windows.h>
#include "dcwin.h"     // ICON_* ids (shared with the compositor protocol)

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

//
// Present every loop iteration (the primary is volatile). The pointer is
// composited into the back buffer via a 16x16 save-under and the whole frame is
// sent to the primary in ONE blit, so the cursor never flickers and moving it
// never forces a desktop recomposite. Pass backRecomposited=TRUE on frames where
// you just fully repainted the back buffer (Render). Returns TRUE if a surface
// was lost+restored (back-buffer content gone; caller must re-render).
//
BOOL GfxPresent(int cursorX, int cursorY, BOOL showCursor, BOOL backRecomposited);

//
// 16x16 color-keyed icons (built from embedded art into DDraw surfaces). Blit in
// the fills pass (it's a Blt, not GDI). Ids are shared with the DCOP_ICON command.
//
void GfxIcon(int id, int x, int y);      // 16x16
void GfxIconBig(int id, int x, int y);   // 32x32

//
// Launch an app: hand the exclusive display off, wait for it, reclaim it.
//
void GfxLaunch(const WCHAR *path);

extern HFONT g_FontUI;
extern HFONT g_FontBold;
extern HFONT g_FontTitle;

#endif // DCGFX_H
