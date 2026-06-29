//
// dcwboot.c - the DCWin boot screen. Baked as HKLM\init Autorun, it runs BEFORE the desktop:
// a fullscreen scene (Dreamcast swirl + wordmark + a live status checklist for network adapter,
// DHCP address, external storage), drawn with the SAME proven Direct3D/PVR2 path dcshell uses
// (dcgfx: GfxFill colour-fills + GfxText glyph-atlas quads + GfxPresent flip). GWES here has no
// FillRect/pens/shapes, so everything is fills + atlas text. Network/DHCP results arrive from the
// netif shim via the DCBOOT shared section (dcboot.h); storage is probed live (GetDiskFreeSpace
// triggers the SD mount). When the checklist finishes it releases the display and launches
// dcshell.exe.
//
#include <windows.h>
#include <winsock.h>
#include "dcgfx.h"          // GfxInit/GfxFill/GfxText/GfxIconBig/GfxPresent + g_Font* + ICON_*
#include "dcboot.h"

#define C_BG     RGB(10, 15, 26)
#define C_TRACK  RGB(29, 41, 66)
#define C_ORANGE RGB(255, 122, 24)
#define C_WHITE  RGB(231, 236, 245)
#define C_MUTED  RGB(111, 129, 158)
#define C_SUBTLE RGB(94, 111, 138)
#define C_GREEN  RGB(93, 202, 165)
#define C_BLUE   RGB(133, 183, 235)
#define C_BLUEDK RGB(60, 96, 150)
#define C_RED    RGB(226, 75, 74)

// kinds: 0=instant, 1=net adapter, 2=DHCP, 3=storage, 4=launch desktop
typedef struct { const WCHAR *label; int kind; int state; DWORD start; WCHAR result[DCB_RESLEN]; } Stage;

static Stage s_stage[] = {
    { L"Starting Windows CE",       0, DCB_PENDING, 0, L"" },
    { L"Initializing display",      0, DCB_PENDING, 0, L"PVR2" },
    { L"Network adapter",           1, DCB_PENDING, 0, L"" },
    { L"Acquiring network address", 2, DCB_PENDING, 0, L"" },
    { L"External storage",          3, DCB_PENDING, 0, L"" },
    { L"Loading desktop",           4, DCB_PENDING, 0, L"" },
};
#define NSTAGE  (int)(sizeof(s_stage) / sizeof(s_stage[0]))
#define NROWS   (NSTAGE - 1)            // the "loading desktop" stage isn't a checklist row

#define MINDWELL  500                   // each stage visible at least this long (ms)
#define NET_TMO  4000
#define ADDR_TMO 8000

static int s_cur, s_skip, s_launch, s_netKicked, s_storeOk;

static void KickNetwork(void)           // force the comm stack (winsock->microstk->mppp) up
{
    WSADATA wsa; SOCKET s;
    if (s_netKicked) return;
    s_netKicked = 1;
    if (WSAStartup(0x0101, &wsa) != 0) return;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s != INVALID_SOCKET) closesocket(s);
}

// Probe \External Storage live: FindFirstFile routes through our FSD (V_FindFirstFile ->
// f_opendir), which mounts the card (SdInit over SPI) on first access. Presence = a valid handle
// or an empty-but-mounted volume; the capacity string is published into DCBOOT by the SD driver
// once it mounts (it knows the sector count), so we read it back here.
static int ProbeStorage(WCHAR *out)
{
    WIN32_FIND_DATAW fd;
    DcBootShared *db;
    HANDLE h = FindFirstFileW(L"\\External Storage\\*", &fd);
    int present = (h != INVALID_HANDLE_VALUE) || (GetLastError() == ERROR_NO_MORE_FILES);
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    if (!present) { lstrcpyW(out, L"no card"); return 0; }
    db = DcBootMap(0);
    if (db && db->state[DCB_STORE] == DCB_OK && db->result[DCB_STORE][0])
        lstrcpyW(out, db->result[DCB_STORE]);   // capacity from the SD driver
    else
        lstrcpyW(out, L"ready");
    return 1;
}

static void StepStages(void)
{
    DcBootShared *db = DcBootMap(0);
    Stage *st;
    DWORD  now = GetTickCount(), el;
    if (s_cur >= NSTAGE) return;
    st = &s_stage[s_cur];

    if (st->state == DCB_PENDING)
    {
        st->state = DCB_ACTIVE;
        st->start = now;
        if (st->kind == 1) KickNetwork();
        else if (st->kind == 3) s_storeOk = ProbeStorage(st->result);
    }
    el = now - st->start;
    switch (st->kind)
    {
    case 0: if (el >= MINDWELL) st->state = DCB_OK; break;
    case 1:
        if (db && db->state[DCB_NET] == DCB_OK)  { lstrcpyW(st->result, db->result[DCB_NET]);  if (el >= MINDWELL) st->state = DCB_OK; }
        else if (el >= NET_TMO)  { lstrcpyW(st->result, L"none");    st->state = DCB_FAIL; }
        break;
    case 2:
        if (db && db->state[DCB_ADDR] == DCB_OK) { lstrcpyW(st->result, db->result[DCB_ADDR]); if (el >= MINDWELL) st->state = DCB_OK; }
        else if (el >= ADDR_TMO) { lstrcpyW(st->result, L"no DHCP"); st->state = DCB_FAIL; }
        break;
    case 3:
        if (el >= MINDWELL) st->state = s_storeOk ? DCB_OK : DCB_FAIL;
        break;
    case 4: if (el >= 700) s_launch = 1; break;
    }
    if (st->state == DCB_OK || st->state == DCB_FAIL) s_cur++;
    if (s_skip)                         // fast-forward to the desktop launch
        while (s_cur < NSTAGE && s_stage[s_cur].kind != 4) { s_stage[s_cur].state = DCB_OK; s_cur++; }
}

// One Windows-flag pane, sheared right (top edge pushed right of the bottom) for the classic
// waving lean. GWES has only colour-fills, so we draw it as a stack of horizontal slabs - this
// is the "ASCII/block art" of the real Windows CE logo, no bitmap blit needed.
static void FlagPane(int x, int y, int w, int h, int shear, COLORREF c)
{
    int i, slabs = 5, sh = h / 5;
    for (i = 0; i < slabs; i++)
    {
        int dy = y + i * sh;
        int dx = x + (shear * (slabs - 1 - i)) / (slabs - 1);   // upper slabs leaned right
        GfxFill(dx, dy, dx + w, dy + sh, c);
    }
}

// The 4-pane Microsoft flag (red/green/blue/yellow), the heart of the "Windows CE" logo.
static void DrawFlag(int x, int y)
{
    int pw = 22, ph = 20, gap = 5, sh = 6;
    FlagPane(x,            y + 3,            pw, ph, sh, RGB(242,  80,  34));   // red    (top-left)
    FlagPane(x + pw + gap, y,                pw, ph, sh, RGB(127, 186,   0));   // green  (top-right)
    FlagPane(x,            y + ph + gap + 3, pw, ph, sh, RGB(  0, 164, 239));   // blue   (bottom-left)
    FlagPane(x + pw + gap, y + ph + gap,     pw, ph, sh, RGB(255, 185,   0));   // yellow (bottom-right)
}

static COLORREF MarkColor(int state, DWORD t)
{
    if (state == DCB_OK)     return C_GREEN;
    if (state == DCB_FAIL)   return C_RED;
    if (state == DCB_ACTIVE) return ((t / 280) & 1) ? C_BLUE : C_BLUEDK;   // pulse
    return C_SUBTLE;
}

static void Render(DWORD t)
{
    HDC dc;
    int i, y, done = 0, cx = SCREEN_W / 2;
    int listX = cx - 175, listW = 350, listTop = 250, row = 30;
    int flagW = 22 + 5 + 22 + 6;                                    // flag glyph width
    int wordW = GfxTextWidth(g_FontBold, L"Windows CE");
    int logoX = cx - (flagW + 14 + wordW) / 2;                      // [flag] 14px [Windows CE]

    GfxFill(0, 0, SCREEN_W, SCREEN_H, C_BG);                         // background
    DrawFlag(logoX, 84);                                            // Windows CE flag (block art)
    GfxFill(listX, 168, listX + listW, 169, C_TRACK);              // thin divider under the logo

    for (i = 0; i < NSTAGE; i++)                                    // status mark squares
    {
        if (s_stage[i].kind == 4) continue;
        y = listTop + i * row;
        GfxFill(listX, y + 3, listX + 10, y + 13, MarkColor(s_stage[i].state, t));
        if (s_stage[i].state == DCB_OK || s_stage[i].state == DCB_FAIL) done++;
    }

    GfxFill(listX, 432, listX + listW, 438, C_TRACK);              // progress track
    GfxFill(listX, 432, listX + (listW * done) / NROWS, 438, C_ORANGE);

    dc = GfxLockDC();
    GfxText(dc, logoX + flagW + 14, 96, C_WHITE, C_BG, g_FontBold, L"Windows CE");
    GfxText(dc, cx - GfxTextWidth(g_FontUI, L"Dreamcast Community Edition") / 2, 140,
            C_BLUE, C_BG, g_FontUI, L"Dreamcast Community Edition");
    for (i = 0; i < NSTAGE; i++)
    {
        Stage *st = &s_stage[i];
        COLORREF lc;
        if (st->kind == 4) continue;
        y = listTop + i * row;
        lc = st->state == DCB_ACTIVE ? C_WHITE : (st->state == DCB_PENDING ? C_SUBTLE : C_MUTED);
        GfxText(dc, listX + 22, y, lc, C_BG, g_FontUI, st->label);
        if (st->result[0])
            GfxText(dc, listX + listW - GfxTextWidth(g_FontUI, st->result), y,
                    st->state == DCB_FAIL ? C_RED : C_BLUE, C_BG, g_FontUI, st->result);
    }
    GfxText(dc, listX, 446, C_SUBTLE, C_BG, g_FontUI, L"SH-4 - retail");
    GfxText(dc, listX + listW - GfxTextWidth(g_FontUI, L"DCWin"), 446, C_SUBTLE, C_BG, g_FontUI, L"DCWin");
    GfxUnlockDC(dc);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) { s_skip = 1; return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    WNDCLASSW wc;
    HWND      hwnd;
    MSG       msg;
    PROCESS_INFORMATION pi;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"DCWBOOT";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, L"DCWBOOT", L"DCWin", WS_VISIBLE, 0, 0, SCREEN_W, SCREEN_H, NULL, NULL, hInst, NULL);

    if (hwnd && GfxInit(hwnd))
    {
        while (!s_launch)
        {
            while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            StepStages();
            Render(GetTickCount());
            GfxPresent(0, 0, FALSE);            // no cursor on the boot screen
            GfxWaitVBlank();
        }
        GfxShutdown();                          // release the exclusive display for dcshell
    }

    // Hand off to the desktop.
    if (!CreateProcessW(L"\\Windows\\dcshell.exe", L"", 0, 0, 0, 0, 0, 0, 0, &pi))
        CreateProcessW(L"dcshell.exe", L"", 0, 0, 0, 0, 0, 0, 0, &pi);
    return 0;
}
