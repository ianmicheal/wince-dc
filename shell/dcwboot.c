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
#include "dcgfx.h" // GfxInit/GfxFill/GfxText/GfxIconBig/GfxPresent + g_Font* + ICON_*
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
typedef struct
{
	const WCHAR *label;
	int kind;
	int state;
	DWORD start;
	WCHAR result[DCB_RESLEN];
} Stage;

static Stage s_aStage[] = {
    {L"Starting Windows CE", 0, DCB_PENDING, 0, L""},
    {L"Initializing display", 0, DCB_PENDING, 0, L"PVR2"},
    {L"Network adapter", 1, DCB_PENDING, 0, L""},
    {L"Acquiring network address", 2, DCB_PENDING, 0, L""},
    {L"External storage", 3, DCB_PENDING, 0, L""},
    {L"Loading desktop", 4, DCB_PENDING, 0, L""},
};
#define NSTAGE   (int)(sizeof(s_aStage) / sizeof(s_aStage[0]))
#define NROWS    (NSTAGE - 1) // the "loading desktop" stage isn't a checklist row

#define MINDWELL 500 // each stage visible at least this long (ms)
#define NET_TMO  4000
#define ADDR_TMO 8000

static int s_nCur, s_bSkip, s_bLaunch, s_bNetKicked, s_bStoreOk;

static void KickNetwork(void) // force the comm stack (winsock->microstk->mppp) up
{
	WSADATA wsa;
	SOCKET s;
	if (s_bNetKicked)
		return;
	s_bNetKicked = 1;
	if (WSAStartup(0x0101, &wsa) != 0)
		return;
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s != INVALID_SOCKET)
		closesocket(s);
}

// Probe \External Storage live: FindFirstFile routes through our FSD (V_FindFirstFile ->
// f_opendir), which mounts the card (SdInit over SPI) on first access. Presence = a valid handle
// or an empty-but-mounted volume; the capacity string is published into DCBOOT by the SD driver
// once it mounts (it knows the sector count), so we read it back here.
static int ProbeStorage(WCHAR *pszOut)
{
	WIN32_FIND_DATAW fd;
	DcBootShared *pDb;
	HANDLE h = FindFirstFileW(L"\\External Storage\\*", &fd);
	int bPresent = (h != INVALID_HANDLE_VALUE) || (GetLastError() == ERROR_NO_MORE_FILES);
	if (h != INVALID_HANDLE_VALUE)
		FindClose(h);
	if (!bPresent)
	{
		lstrcpyW(pszOut, L"no card");
		return 0;
	}
	pDb = DcBootMap(0);
	if (pDb && pDb->state[DCB_STORE] == DCB_OK && pDb->result[DCB_STORE][0])
		lstrcpyW(pszOut, pDb->result[DCB_STORE]); // capacity from the SD driver
	else
		lstrcpyW(pszOut, L"ready");
	return 1;
}

static void StepStages(void)
{
	DcBootShared *pDb = DcBootMap(0);
	Stage *pSt;
	DWORD dwNow = GetTickCount(), dwEl;
	if (s_nCur >= NSTAGE)
		return;
	pSt = &s_aStage[s_nCur];

	if (pSt->state == DCB_PENDING)
	{
		pSt->state = DCB_ACTIVE;
		pSt->start = dwNow;
		if (pSt->kind == 1)
			KickNetwork();
		else if (pSt->kind == 3)
			s_bStoreOk = ProbeStorage(pSt->result);
	}
	dwEl = dwNow - pSt->start;
	switch (pSt->kind)
	{
		case 0:
			if (dwEl >= MINDWELL)
				pSt->state = DCB_OK;
			break;
		case 1:
			if (pDb && pDb->state[DCB_NET] == DCB_OK)
			{
				lstrcpyW(pSt->result, pDb->result[DCB_NET]);
				if (dwEl >= MINDWELL)
					pSt->state = DCB_OK;
			}
			else if (dwEl >= NET_TMO)
			{
				lstrcpyW(pSt->result, L"none");
				pSt->state = DCB_FAIL;
			}
			break;
		case 2:
			if (pDb && pDb->state[DCB_ADDR] == DCB_OK)
			{
				lstrcpyW(pSt->result, pDb->result[DCB_ADDR]);
				if (dwEl >= MINDWELL)
					pSt->state = DCB_OK;
			}
			else if (dwEl >= ADDR_TMO)
			{
				lstrcpyW(pSt->result, L"no DHCP");
				pSt->state = DCB_FAIL;
			}
			break;
		case 3:
			if (dwEl >= MINDWELL)
				pSt->state = s_bStoreOk ? DCB_OK : DCB_FAIL;
			break;
		case 4:
			if (dwEl >= 700)
				s_bLaunch = 1;
			break;
	}
	if (pSt->state == DCB_OK || pSt->state == DCB_FAIL)
		s_nCur++;
	if (s_bSkip) // fast-forward to the desktop launch
		while (s_nCur < NSTAGE && s_aStage[s_nCur].kind != 4)
		{
			s_aStage[s_nCur].state = DCB_OK;
			s_nCur++;
		}
}

// One Windows-flag pane, sheared right (top edge pushed right of the bottom) for the classic
// waving lean. GWES has only colour-fills, so we draw it as a stack of horizontal slabs - this
// is the "ASCII/block art" of the real Windows CE logo, no bitmap blit needed.
static void FlagPane(int x, int y, int w, int h, int shear, COLORREF c)
{
	int i, nSlabs = 5, nSh = h / 5;
	for (i = 0; i < nSlabs; i++)
	{
		int dy = y + i * nSh;
		int dx = x + (shear * (nSlabs - 1 - i)) / (nSlabs - 1); // upper slabs leaned right
		GfxFill(dx, dy, dx + w, dy + nSh, c);
	}
}

// The 4-pane Microsoft flag (red/green/blue/yellow), the heart of the "Windows CE" logo.
static void DrawFlag(int x, int y)
{
	int nPw = 22, nPh = 20, nGap = 5, nSh = 6;
	FlagPane(x, y + 3, nPw, nPh, nSh, RGB(242, 80, 34));              // red    (top-left)
	FlagPane(x + nPw + nGap, y, nPw, nPh, nSh, RGB(127, 186, 0));     // green  (top-right)
	FlagPane(x, y + nPh + nGap + 3, nPw, nPh, nSh, RGB(0, 164, 239)); // blue   (bottom-left)
	FlagPane(x + nPw + nGap, y + nPh + nGap, nPw, nPh, nSh,
	         RGB(255, 185, 0)); // yellow (bottom-right)
}

static COLORREF MarkColor(int state, DWORD t)
{
	if (state == DCB_OK)
		return C_GREEN;
	if (state == DCB_FAIL)
		return C_RED;
	if (state == DCB_ACTIVE)
		return ((t / 280) & 1) ? C_BLUE : C_BLUEDK; // pulse
	return C_SUBTLE;
}

static void Render(DWORD t)
{
	HDC hdc;
	int i, y, nDone = 0, nCx = SCREEN_W / 2;
	int nListX = nCx - 175, nListW = 350, nListTop = 250, nRow = 30;
	int nFlagW = 22 + 5 + 22 + 6; // flag glyph width
	int nWordW = GfxTextWidth(g_FontBold, L"Windows CE");
	int nLogoX = nCx - (nFlagW + 14 + nWordW) / 2; // [flag] 14px [Windows CE]

	GfxFill(0, 0, SCREEN_W, SCREEN_H, C_BG);             // background
	DrawFlag(nLogoX, 84);                                // Windows CE flag (block art)
	GfxFill(nListX, 168, nListX + nListW, 169, C_TRACK); // thin divider under the logo

	for (i = 0; i < NSTAGE; i++) // status mark squares
	{
		if (s_aStage[i].kind == 4)
			continue;
		y = nListTop + i * nRow;
		GfxFill(nListX, y + 3, nListX + 10, y + 13, MarkColor(s_aStage[i].state, t));
		if (s_aStage[i].state == DCB_OK || s_aStage[i].state == DCB_FAIL)
			nDone++;
	}

	GfxFill(nListX, 432, nListX + nListW, 438, C_TRACK); // progress track
	GfxFill(nListX, 432, nListX + (nListW * nDone) / NROWS, 438, C_ORANGE);

	hdc = GfxLockDC();
	GfxText(hdc, nLogoX + nFlagW + 14, 96, C_WHITE, C_BG, g_FontBold, L"Windows CE");
	GfxText(hdc, nCx - GfxTextWidth(g_FontUI, L"Dreamcast Community Edition") / 2, 140, C_BLUE,
	        C_BG, g_FontUI, L"Dreamcast Community Edition");
	for (i = 0; i < NSTAGE; i++)
	{
		Stage *pSt = &s_aStage[i];
		COLORREF lc;
		if (pSt->kind == 4)
			continue;
		y = nListTop + i * nRow;
		lc = pSt->state == DCB_ACTIVE ? C_WHITE : (pSt->state == DCB_PENDING ? C_SUBTLE : C_MUTED);
		GfxText(hdc, nListX + 22, y, lc, C_BG, g_FontUI, pSt->label);
		if (pSt->result[0])
			GfxText(hdc, nListX + nListW - GfxTextWidth(g_FontUI, pSt->result), y,
			        pSt->state == DCB_FAIL ? C_RED : C_BLUE, C_BG, g_FontUI, pSt->result);
	}
	GfxText(hdc, nListX, 446, C_SUBTLE, C_BG, g_FontUI, L"SH-4 - retail");
	GfxText(hdc, nListX + nListW - GfxTextWidth(g_FontUI, L"DCWin"), 446, C_SUBTLE, C_BG, g_FontUI,
	        L"DCWin");
	GfxUnlockDC(hdc);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_KEYDOWN)
	{
		s_bSkip = 1;
		return 0;
	}
	return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
	WNDCLASSW wc;
	HWND hWnd;
	MSG msg;
	PROCESS_INFORMATION pi;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hbrBackground = NULL;
	wc.lpszClassName = L"DCWBOOT";
	RegisterClassW(&wc);
	hWnd = CreateWindowExW(0, L"DCWBOOT", L"DCWin", WS_VISIBLE, 0, 0, SCREEN_W, SCREEN_H, NULL,
	                       NULL, hInst, NULL);

	if (hWnd && GfxInit(hWnd))
	{
		while (!s_bLaunch)
		{
			while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			StepStages();
			Render(GetTickCount());
			GfxPresent(0, 0, FALSE); // no cursor on the boot screen
			GfxWaitVBlank();
		}
		GfxShutdown(); // release the exclusive display for dcshell
	}

	// Hand off to the desktop.
	if (!CreateProcessW(L"\\Windows\\dcshell.exe", L"", 0, 0, 0, 0, 0, 0, 0, &pi))
		CreateProcessW(L"dcshell.exe", L"", 0, 0, 0, 0, 0, 0, 0, &pi);
	return 0;
}
