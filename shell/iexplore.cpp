//
// iexplore.cpp - DCWin Internet Explorer: a fullscreen Trident browser.
//
// Launched by the shell through the display hand-off (a non-"dcw" name, so dcshell runs it via
// GfxLaunch, which frees the DirectDraw primary + input). The app owns the 640x480 display
// using OUR compositor (dcgfx, the same PVR2/Direct3D path the shell + boot screen use):
//
//   - The stock WebBrowser control (iehost.cpp) is hosted as a child window and renders itself
//     via GWES into the GDI framebuffer. The PVR doesn't scan that out while we hold the DDraw
//     flip primary, so each frame we sample the control's region (GfxBlitPage) and draw it as a
//     compositor quad - exactly the SDK htmlsamp GETGDIINFO trick, wrapped in dcgfx.
//   - The address/status chrome + on-screen keyboard are ordinary dcgfx quads (GfxFill/GfxText).
//
//   D-pad   move link/element focus (and scroll) inside the page
//   A       open the focused link        B  back     Y  forward
//   Start   address bar (on-screen keyboard)         X  exit -> back to the shell
//
#include <windows.h>
#include <keybd.h>           // KeyStateDownFlag / KeyShiftNoCharacterFlag
#include "iehost.h"          // C++ COM host (must precede dcgfx.h; dcgfx.h does not force CINTERFACE)
#include "dcgfx.h"           // our PVR2/D3D compositor (GfxInit/GfxFill/GfxText/GfxBlitPage/GfxPresent)
#include "dcinput.h"

#define SCRW   SCREEN_W
#define SCRH   SCREEN_H
#define TOPH   24            // address bar height
#define BOTH   18            // status bar height

#define HOME_URL  L"about:blank"   // instant white render: isolates the compositor from networking

#define CL_BAR    RGB(192, 192, 192)
#define CL_ADDR   RGB(255, 255, 255)
#define CL_TEXT   RGB(0, 0, 0)
#define CL_SEL    RGB(0, 0, 128)
#define CL_WHITE  RGB(255, 255, 255)

static HWND          g_frame = NULL;
static CBrowserHost *g_host  = NULL;
static int           g_run   = 1;

// chrome state (updated by the Ui* hooks from the event sink)
static WCHAR  g_url[512]    = HOME_URL;
static WCHAR  g_title[256]  = L"";
static WCHAR  g_status[256] = L"Ready";
static int    g_busy   = 0;
static int    g_secure = 0;

// on-screen keyboard / URL-entry state
static int    g_urlMode = 0;
static WCHAR  g_edit[512] = L"";
static int    g_selRow = 0, g_selCol = 0;

// CE 2.12 has no lstrcpyn*; small bounded copy (cap includes the terminator).
static void StrCpyN(WCHAR *dst, const WCHAR *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ---- event-sink hooks (declared in iehost.h) ---------------------------------
extern "C" void UiOnNavigate(const WCHAR *url)  { if (url)    StrCpyN(g_url, url, 512); }
extern "C" void UiOnTitle(const WCHAR *title)   { if (title)  StrCpyN(g_title, title, 256); }
extern "C" void UiOnStatus(const WCHAR *status)
{
    if (!status) return;
    StrCpyN(g_status, status, 256);
    OutputDebugStringW(L"IE status: "); OutputDebugStringW(status); OutputDebugStringW(L"\r\n");
}
extern "C" void UiOnBusy(int busy)   { g_busy = busy; }
extern "C" void UiOnSecure(int sec)  { g_secure = sec; }

// ---- keyboard injection into the control -------------------------------------
// PostKeybdMessage((HWND)-1, ...) posts to the system focus (the control's inner window once it
// is UI-active), which Trident consumes for element navigation.
static void InjectKey(UINT vk)
{
    UINT sc = 0, ch = 0;
    PostKeybdMessage((HWND)-1, vk, KeyStateDownFlag | KeyShiftNoCharacterFlag, 1, &sc, &ch);
    sc = 0; ch = 0;
    PostKeybdMessage((HWND)-1, vk, KeyShiftNoCharacterFlag, 1, &sc, &ch);
}

// ---- on-screen keyboard layout -----------------------------------------------
static const WCHAR *kbRows[4] =
{
    L"1234567890",
    L"qwertyuiop",
    L"asdfghjkl-",
    L"zxcvbnm./_",
};
#define KB_CHARROWS 4
#define KB_COLS     10
static const WCHAR *kbSpecial[6] = { L".com", L"http://", L"space", L"DEL", L"Go", L"Cancel" };
#define KB_SPECIAL  6
#define KB_SPECROW  KB_CHARROWS
#define KCELL_W 58
#define KCELL_H 32
#define KORG_X  16
#define KORG_Y  150
#define KSP_W   100
static int KbRowCount(int row) { return (row == KB_SPECROW) ? KB_SPECIAL : KB_COLS; }

// ---- rendering (all through dcgfx; called once per frame, then GfxPresent) ----
static void RenderPage(void)
{
    HDC dc;
    WCHAR addr[600];
    const WCHAR *st;

    // Force the control to repaint into the GDI framebuffer this frame, then sample it.
    if (g_host && g_host->GetControlWindow())
    {
        InvalidateRect(g_host->GetControlWindow(), NULL, FALSE);
        UpdateWindow(g_host->GetControlWindow());
    }
    GfxFill(0, 0, SCRW, TOPH, CL_ADDR);                 // address bar bg
    GfxFill(0, SCRH - BOTH, SCRW, SCRH, CL_BAR);        // status bar bg
    GfxBlitPage(0, TOPH, SCRW, SCRH - TOPH - BOTH,      // the control's GWES output -> quad
                0, TOPH, SCRW, SCRH - TOPH - BOTH);

    dc = GfxLockDC();
    wsprintfW(addr, L"%s%s", g_secure ? L"[secure] " : L"", g_url);
    GfxText(dc, 4, 4, CL_TEXT, CL_ADDR, g_FontUI, addr);
    // While busy, show the live WinInet status (Resolving/Connecting/Opening...) - it pinpoints
    // where a stalled fetch is stuck - falling back to "Loading..." only if none has arrived.
    st = g_busy ? (g_status[0] ? g_status : L"Loading...") : (g_title[0] ? g_title : g_status);
    GfxText(dc, 4, SCRH - BOTH + 1, CL_TEXT, CL_BAR, g_FontUI, st);
    GfxText(dc, SCRW - 272, SCRH - BOTH + 1, CL_TEXT, CL_BAR, g_FontUI,
            L"A:Open B:Back Y:Fwd Start:URL X:Exit");
    GfxUnlockDC(dc);

    GfxPresent(0, 0, FALSE);
}

static void RenderKeyboard(void)
{
    HDC dc;
    int row, col;

    GfxFill(0, 0, SCRW, SCRH, CL_BAR);                  // full background
    GfxFill(0, 0, SCRW, TOPH, CL_ADDR);
    GfxFill(8, 40, SCRW - 8, 68, CL_WHITE);            // URL edit box
    for (row = 0; row < KB_CHARROWS; row++)
        for (col = 0; col < KB_COLS; col++)
        {
            int sel = (row == g_selRow && col == g_selCol);
            int x = KORG_X + col * (KCELL_W + 2), y = KORG_Y + row * (KCELL_H + 2);
            GfxFill(x, y, x + KCELL_W, y + KCELL_H, sel ? CL_SEL : CL_ADDR);
        }
    for (col = 0; col < KB_SPECIAL; col++)
    {
        int sel = (g_selRow == KB_SPECROW && col == g_selCol);
        int x = KORG_X + col * (KSP_W + 2), y = KORG_Y + KB_CHARROWS * (KCELL_H + 2);
        GfxFill(x, y, x + KSP_W, y + KCELL_H, sel ? CL_SEL : CL_ADDR);
    }

    dc = GfxLockDC();
    GfxText(dc, 4, 4, CL_TEXT, CL_ADDR, g_FontUI, L"Enter address:");
    GfxText(dc, 12, 46, CL_TEXT, CL_WHITE, g_FontUI, g_edit[0] ? g_edit : L"_");
    for (row = 0; row < KB_CHARROWS; row++)
        for (col = 0; col < KB_COLS; col++)
        {
            WCHAR k[2]; int sel = (row == g_selRow && col == g_selCol);
            int x = KORG_X + col * (KCELL_W + 2), y = KORG_Y + row * (KCELL_H + 2);
            k[0] = kbRows[row][col]; k[1] = 0;
            GfxText(dc, x + KCELL_W / 2 - 4, y + 8, sel ? CL_WHITE : CL_TEXT,
                    sel ? CL_SEL : CL_ADDR, g_FontUI, k);
        }
    for (col = 0; col < KB_SPECIAL; col++)
    {
        int sel = (g_selRow == KB_SPECROW && col == g_selCol);
        int x = KORG_X + col * (KSP_W + 2), y = KORG_Y + KB_CHARROWS * (KCELL_H + 2);
        GfxText(dc, x + 6, y + 8, sel ? CL_WHITE : CL_TEXT, sel ? CL_SEL : CL_ADDR,
                g_FontUI, kbSpecial[col]);
    }
    GfxUnlockDC(dc);

    GfxPresent(0, 0, FALSE);
}

// ---- URL editing -------------------------------------------------------------
static void EditAppend(const WCHAR *s)
{
    int n = lstrlenW(g_edit), m = lstrlenW(s);
    if (n + m < 510) lstrcpyW(g_edit + n, s);
}
static void EditBackspace(void)
{
    int n = lstrlenW(g_edit);
    if (n > 0) g_edit[n - 1] = 0;
}
static void EnterUrlMode(void)
{
    StrCpyN(g_edit, g_url, 512);
    g_selRow = 0; g_selCol = 0;
    g_urlMode = 1;
    if (g_host && g_host->GetControlWindow())
        ShowWindow(g_host->GetControlWindow(), SW_HIDE);
}
static void LeaveUrlMode(int navigate)
{
    g_urlMode = 0;
    if (g_host && g_host->GetControlWindow())
        ShowWindow(g_host->GetControlWindow(), SW_SHOW);
    if (navigate && g_edit[0] && g_host)
    {
        WCHAR url[540];
        int i, hasScheme = 0;
        for (i = 0; g_edit[i] && i < 12; i++) if (g_edit[i] == L':') { hasScheme = 1; break; }
        if (hasScheme) StrCpyN(url, g_edit, 540);
        else { lstrcpyW(url, L"http://"); StrCpyN(url + 7, g_edit, 533); }
        StrCpyN(g_url, url, 512);
        g_busy = 1;
        g_host->LoadUrl(url);                // fetch over winsock + render (WinInet is dead here)
    }
}
static void KbMove(UINT vk)
{
    int rows = KB_SPECROW + 1, n;
    if (vk == VK_UP)    g_selRow = (g_selRow + rows - 1) % rows;
    if (vk == VK_DOWN)  g_selRow = (g_selRow + 1) % rows;
    n = KbRowCount(g_selRow);
    if (vk == VK_LEFT)  g_selCol = (g_selCol + n - 1) % n;
    if (vk == VK_RIGHT) g_selCol = (g_selCol + 1) % n;
    if (g_selCol >= n)  g_selCol = n - 1;
}
static void KbPress(void)
{
    if (g_selRow < KB_CHARROWS)
    {
        WCHAR k[2]; k[0] = kbRows[g_selRow][g_selCol]; k[1] = 0;
        EditAppend(k);
    }
    else switch (g_selCol)
    {
    case 0: EditAppend(L".com"); break;
    case 1: if (!g_edit[0]) EditAppend(L"http://"); break;
    case 2: EditAppend(L" "); break;
    case 3: EditBackspace(); break;
    case 4: LeaveUrlMode(1); break;
    case 5: LeaveUrlMode(0); break;
    }
}

// ---- per-frame controller handling -------------------------------------------
static void PollUrlMode(void)
{
    DWORD vk;
    while (DInNextKey(&vk))
    {
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) KbMove(vk);
        else if (vk == VK_RETURN)                                            KbPress();
        else if (vk == VK_BACK || vk == VK_ESCAPE)                           EditBackspace();
    }
    if (DInButtonEdge(DIN_BTN_A)) KbPress();
    if (DInButtonEdge(DIN_BTN_B)) EditBackspace();
    if (DInButtonEdge(DIN_BTN_X) || DInButtonEdge(DIN_BTN_START)) LeaveUrlMode(0);
    (void)DInButtonEdge(DIN_BTN_Y);
}
static void PollPageMode(void)
{
    DWORD vk;
    while (DInNextKey(&vk)) InjectKey(vk);          // arrows -> element focus + scroll in the control
    if (DInButtonEdge(DIN_BTN_A))     InjectKey(VK_RETURN);
    if (DInButtonEdge(DIN_BTN_B))     { if (g_host) g_host->GoBack(); }
    if (DInButtonEdge(DIN_BTN_Y))     { if (g_host) g_host->GoForward(); }
    if (DInButtonEdge(DIN_BTN_START)) EnterUrlMode();
    if (DInButtonEdge(DIN_BTN_X))     { g_run = 0; PostMessageW(g_frame, WM_CLOSE, 0, 0); }
}

// ---- frame window ------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;                  // dcgfx owns the surface
    case WM_PAINT:      { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); } return 0;
    case WM_SETFOCUS:
        if (g_host && g_host->GetControlWindow() && !g_urlMode)
            SetFocus(g_host->GetControlWindow());
        return 0;
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    WNDCLASSW wc;
    MSG       msg;
    RECT      rc;

    (void)hPrev; (void)nShow; (void)lpCmd;
    OutputDebugStringW(L"IE: WinMain enter\r\n");

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
    {
        OutputDebugStringW(L"IE: CoInitializeEx FAILED\r\n");
        return 1;
    }
    LoadLibraryW(L"ieceui.dll");                    // register the IE CE controls (per htmlsamp)

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"DCIEXPLORE";
    RegisterClassW(&wc);
    g_frame = CreateWindowExW(0, L"DCIEXPLORE", L"Internet Explorer",
                              WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, SCRW, SCRH, NULL, NULL, hInst, NULL);
    if (!g_frame) { CoUninitialize(); return 1; }

    if (!GfxInit(g_frame))                          // take the display via our compositor
    {
        OutputDebugStringW(L"IE: GfxInit FAILED\r\n");
        CoUninitialize();
        return 1;
    }
    GfxInitPageLayer();                             // wrap the GWES framebuffer for the page quad

    rc.left = 0; rc.top = TOPH; rc.right = SCRW; rc.bottom = SCRH - BOTH;
    g_host = new CBrowserHost(g_frame, &rc);
    if (!g_host->Create())
    {
        OutputDebugStringW(L"IE: host Create FAILED\r\n");
        lstrcpyW(g_status, L"Failed to create the browser control");
    }
    else
    {
        g_host->Navigate(HOME_URL);
        if (g_host->GetControlWindow())
        {
            ShowWindow(g_host->GetControlWindow(), SW_SHOW);
            InvalidateRect(g_host->GetControlWindow(), NULL, TRUE);
            UpdateWindow(g_host->GetControlWindow());
        }
    }

    DInInit(g_frame);                               // our own DI (shell handed input off)

    while (g_run)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_run = 0; break; }
            if (g_host && !g_urlMode)               // let the control handle its own keyboard msgs
            {
                HWND cw = g_host->GetControlWindow();
                if (cw && (msg.hwnd == cw || IsChild(cw, msg.hwnd)) &&
                    msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST &&
                    g_host->TranslateAccel(&msg) == S_OK)
                    continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_run) break;

        DInUpdate();
        if (g_urlMode) PollUrlMode(); else PollPageMode();
        if (g_host) g_host->PumpPending();          // winsock-load any link the control tried to open

        if (g_urlMode) RenderKeyboard(); else RenderPage();
        GfxWaitVBlank();
    }

    if (g_host) { g_host->Destroy(); g_host->Release(); g_host = NULL; }
    DInShutdown();
    GfxShutdown();
    CoUninitialize();
    OutputDebugStringW(L"IE: exit\r\n");
    return 0;
}
