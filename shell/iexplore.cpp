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
#include <keybd.h>  // KeyStateDownFlag / KeyShiftNoCharacterFlag
#include "iehost.h" // C++ COM host (must precede dcgfx.h; dcgfx.h does not force CINTERFACE)
#include "dcgfx.h"  // our PVR2/D3D compositor (GfxInit/GfxFill/GfxText/GfxBlitPage/GfxPresent)
#include "dcinput.h"

#define SCRW     SCREEN_W
#define SCRH     SCREEN_H
#define TOPH     24 // address bar height
#define BOTH     18 // status bar height

#define HOME_URL L"about:blank" // instant white render: isolates the compositor from networking

#define CL_BAR   RGB(192, 192, 192)
#define CL_ADDR  RGB(255, 255, 255)
#define CL_TEXT  RGB(0, 0, 0)
#define CL_SEL   RGB(0, 0, 128)
#define CL_WHITE RGB(255, 255, 255)

static HWND g_hwndFrame = NULL;
static CBrowserHost *g_pHost = NULL;
static int g_nRun = 1;

// chrome state (updated by the Ui* hooks from the event sink)
static WCHAR g_szUrl[512] = HOME_URL;
static WCHAR g_szTitle[256] = L"";
static WCHAR g_szStatus[256] = L"Ready";
static int g_nBusy = 0;
static int g_nSecure = 0;

// on-screen keyboard / URL-entry state
static int g_nUrlMode = 0;
static WCHAR g_szEdit[512] = L"";
static int g_nSelRow = 0, g_nSelCol = 0;

// CE 2.12 has no lstrcpyn*; small bounded copy (cap includes the terminator).
static void StrCpyN(WCHAR *pszDst, const WCHAR *pszSrc, int cap)
{
	int i = 0;
	if (cap <= 0)
		return;
	for (; pszSrc[i] && i < cap - 1; i++)
		pszDst[i] = pszSrc[i];
	pszDst[i] = 0;
}

// ---- event-sink hooks (declared in iehost.h) ---------------------------------
extern "C" void UiOnNavigate(const WCHAR *psz)
{
	if (psz)
		StrCpyN(g_szUrl, psz, 512);
}
extern "C" void UiOnTitle(const WCHAR *psz)
{
	if (psz)
		StrCpyN(g_szTitle, psz, 256);
}
extern "C" void UiOnStatus(const WCHAR *psz)
{
	if (!psz)
		return;
	StrCpyN(g_szStatus, psz, 256);
	OutputDebugStringW(L"IE status: ");
	OutputDebugStringW(psz);
	OutputDebugStringW(L"\r\n");
}
extern "C" void UiOnBusy(int nBusy)
{
	g_nBusy = nBusy;
}
extern "C" void UiOnSecure(int nSecure)
{
	g_nSecure = nSecure;
}

// ---- keyboard injection into the control -------------------------------------
// PostKeybdMessage((HWND)-1, ...) posts to the system focus (the control's inner window once it
// is UI-active), which Trident consumes for element navigation.
static void InjectKey(UINT vk)
{
	UINT sc = 0, ch = 0;
	PostKeybdMessage((HWND)-1, vk, KeyStateDownFlag | KeyShiftNoCharacterFlag, 1, &sc, &ch);
	sc = 0;
	ch = 0;
	PostKeybdMessage((HWND)-1, vk, KeyShiftNoCharacterFlag, 1, &sc, &ch);
}

// ---- on-screen keyboard layout -----------------------------------------------
static const WCHAR *g_apszKbRows[4] = {
    L"1234567890",
    L"qwertyuiop",
    L"asdfghjkl-",
    L"zxcvbnm./_",
};
#define KB_CHARROWS 4
#define KB_COLS     10
static const WCHAR *g_apszKbSpecial[6] = {L".com", L"http://", L"space", L"DEL", L"Go", L"Cancel"};
#define KB_SPECIAL 6
#define KB_SPECROW KB_CHARROWS
#define KCELL_W    58
#define KCELL_H    32
#define KORG_X     16
#define KORG_Y     150
#define KSP_W      100
static int KbRowCount(int nRow)
{
	return (nRow == KB_SPECROW) ? KB_SPECIAL : KB_COLS;
}

// ---- rendering (all through dcgfx; called once per frame, then GfxPresent) ----
static void RenderPage(void)
{
	HDC hdc;
	WCHAR szAddr[600];
	const WCHAR *pszStatus;

	// Force the control to repaint into the GDI framebuffer this frame, then sample it.
	if (g_pHost && g_pHost->GetControlWindow())
	{
		InvalidateRect(g_pHost->GetControlWindow(), NULL, FALSE);
		UpdateWindow(g_pHost->GetControlWindow());
	}
	GfxFill(0, 0, SCRW, TOPH, CL_ADDR);            // address bar bg
	GfxFill(0, SCRH - BOTH, SCRW, SCRH, CL_BAR);   // status bar bg
	GfxBlitPage(0, TOPH, SCRW, SCRH - TOPH - BOTH, // the control's GWES output -> quad
	            0, TOPH, SCRW, SCRH - TOPH - BOTH);

	hdc = GfxLockDC();
	wsprintfW(szAddr, L"%s%s", g_nSecure ? L"[secure] " : L"", g_szUrl);
	GfxText(hdc, 4, 4, CL_TEXT, CL_ADDR, g_FontUI, szAddr);
	// While busy, show the live WinInet status (Resolving/Connecting/Opening...) - it pinpoints
	// where a stalled fetch is stuck - falling back to "Loading..." only if none has arrived.
	pszStatus = g_nBusy ? (g_szStatus[0] ? g_szStatus : L"Loading...")
	                    : (g_szTitle[0] ? g_szTitle : g_szStatus);
	GfxText(hdc, 4, SCRH - BOTH + 1, CL_TEXT, CL_BAR, g_FontUI, pszStatus);
	GfxText(hdc, SCRW - 272, SCRH - BOTH + 1, CL_TEXT, CL_BAR, g_FontUI,
	        L"A:Open B:Back Y:Fwd Start:URL X:Exit");
	GfxUnlockDC(hdc);

	GfxPresent(0, 0, FALSE);
}

static void RenderKeyboard(void)
{
	HDC hdc;
	int iRow, iCol;

	GfxFill(0, 0, SCRW, SCRH, CL_BAR); // full background
	GfxFill(0, 0, SCRW, TOPH, CL_ADDR);
	GfxFill(8, 40, SCRW - 8, 68, CL_WHITE); // URL edit box
	for (iRow = 0; iRow < KB_CHARROWS; iRow++)
		for (iCol = 0; iCol < KB_COLS; iCol++)
		{
			int bSel = (iRow == g_nSelRow && iCol == g_nSelCol);
			int x = KORG_X + iCol * (KCELL_W + 2), y = KORG_Y + iRow * (KCELL_H + 2);
			GfxFill(x, y, x + KCELL_W, y + KCELL_H, bSel ? CL_SEL : CL_ADDR);
		}
	for (iCol = 0; iCol < KB_SPECIAL; iCol++)
	{
		int bSel = (g_nSelRow == KB_SPECROW && iCol == g_nSelCol);
		int x = KORG_X + iCol * (KSP_W + 2), y = KORG_Y + KB_CHARROWS * (KCELL_H + 2);
		GfxFill(x, y, x + KSP_W, y + KCELL_H, bSel ? CL_SEL : CL_ADDR);
	}

	hdc = GfxLockDC();
	GfxText(hdc, 4, 4, CL_TEXT, CL_ADDR, g_FontUI, L"Enter address:");
	GfxText(hdc, 12, 46, CL_TEXT, CL_WHITE, g_FontUI, g_szEdit[0] ? g_szEdit : L"_");
	for (iRow = 0; iRow < KB_CHARROWS; iRow++)
		for (iCol = 0; iCol < KB_COLS; iCol++)
		{
			WCHAR k[2];
			int bSel = (iRow == g_nSelRow && iCol == g_nSelCol);
			int x = KORG_X + iCol * (KCELL_W + 2), y = KORG_Y + iRow * (KCELL_H + 2);
			k[0] = g_apszKbRows[iRow][iCol];
			k[1] = 0;
			GfxText(hdc, x + KCELL_W / 2 - 4, y + 8, bSel ? CL_WHITE : CL_TEXT,
			        bSel ? CL_SEL : CL_ADDR, g_FontUI, k);
		}
	for (iCol = 0; iCol < KB_SPECIAL; iCol++)
	{
		int bSel = (g_nSelRow == KB_SPECROW && iCol == g_nSelCol);
		int x = KORG_X + iCol * (KSP_W + 2), y = KORG_Y + KB_CHARROWS * (KCELL_H + 2);
		GfxText(hdc, x + 6, y + 8, bSel ? CL_WHITE : CL_TEXT, bSel ? CL_SEL : CL_ADDR, g_FontUI,
		        g_apszKbSpecial[iCol]);
	}
	GfxUnlockDC(hdc);

	GfxPresent(0, 0, FALSE);
}

// ---- URL editing -------------------------------------------------------------
static void EditAppend(const WCHAR *psz)
{
	int nEdit = lstrlenW(g_szEdit), nAdd = lstrlenW(psz);
	if (nEdit + nAdd < 510)
		lstrcpyW(g_szEdit + nEdit, psz);
}
static void EditBackspace(void)
{
	int nEdit = lstrlenW(g_szEdit);
	if (nEdit > 0)
		g_szEdit[nEdit - 1] = 0;
}
static void EnterUrlMode(void)
{
	StrCpyN(g_szEdit, g_szUrl, 512);
	g_nSelRow = 0;
	g_nSelCol = 0;
	g_nUrlMode = 1;
	if (g_pHost && g_pHost->GetControlWindow())
		ShowWindow(g_pHost->GetControlWindow(), SW_HIDE);
}
static void LeaveUrlMode(int bNavigate)
{
	g_nUrlMode = 0;
	if (g_pHost && g_pHost->GetControlWindow())
		ShowWindow(g_pHost->GetControlWindow(), SW_SHOW);
	if (bNavigate && g_szEdit[0] && g_pHost)
	{
		WCHAR szUrl[540];
		int i, bHasScheme = 0;
		for (i = 0; g_szEdit[i] && i < 12; i++)
			if (g_szEdit[i] == L':')
			{
				bHasScheme = 1;
				break;
			}
		if (bHasScheme)
			StrCpyN(szUrl, g_szEdit, 540);
		else
		{
			lstrcpyW(szUrl, L"http://");
			StrCpyN(szUrl + 7, g_szEdit, 533);
		}
		StrCpyN(g_szUrl, szUrl, 512);
		g_nBusy = 1;
		g_pHost->LoadUrl(szUrl); // fetch over winsock + render (WinInet is dead here)
	}
}
static void KbMove(UINT vk)
{
	int nRows = KB_SPECROW + 1, n;
	if (vk == VK_UP)
		g_nSelRow = (g_nSelRow + nRows - 1) % nRows;
	if (vk == VK_DOWN)
		g_nSelRow = (g_nSelRow + 1) % nRows;
	n = KbRowCount(g_nSelRow);
	if (vk == VK_LEFT)
		g_nSelCol = (g_nSelCol + n - 1) % n;
	if (vk == VK_RIGHT)
		g_nSelCol = (g_nSelCol + 1) % n;
	if (g_nSelCol >= n)
		g_nSelCol = n - 1;
}
static void KbPress(void)
{
	if (g_nSelRow < KB_CHARROWS)
	{
		WCHAR k[2];
		k[0] = g_apszKbRows[g_nSelRow][g_nSelCol];
		k[1] = 0;
		EditAppend(k);
	}
	else
		switch (g_nSelCol)
		{
			case 0:
				EditAppend(L".com");
				break;
			case 1:
				if (!g_szEdit[0])
					EditAppend(L"http://");
				break;
			case 2:
				EditAppend(L" ");
				break;
			case 3:
				EditBackspace();
				break;
			case 4:
				LeaveUrlMode(1);
				break;
			case 5:
				LeaveUrlMode(0);
				break;
		}
}

// ---- per-frame controller handling -------------------------------------------
static void PollUrlMode(void)
{
	DWORD vk;
	while (DInNextKey(&vk))
	{
		if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)
			KbMove(vk);
		else if (vk == VK_RETURN)
			KbPress();
		else if (vk == VK_BACK || vk == VK_ESCAPE)
			EditBackspace();
	}
	if (DInButtonEdge(DIN_BTN_A))
		KbPress();
	if (DInButtonEdge(DIN_BTN_B))
		EditBackspace();
	if (DInButtonEdge(DIN_BTN_X) || DInButtonEdge(DIN_BTN_START))
		LeaveUrlMode(0);
	(void)DInButtonEdge(DIN_BTN_Y);
}
static void PollPageMode(void)
{
	DWORD vk;
	while (DInNextKey(&vk))
		InjectKey(vk); // arrows -> element focus + scroll in the control
	if (DInButtonEdge(DIN_BTN_A))
		InjectKey(VK_RETURN);
	if (DInButtonEdge(DIN_BTN_B))
	{
		if (g_pHost)
			g_pHost->GoBack();
	}
	if (DInButtonEdge(DIN_BTN_Y))
	{
		if (g_pHost)
			g_pHost->GoForward();
	}
	if (DInButtonEdge(DIN_BTN_START))
		EnterUrlMode();
	if (DInButtonEdge(DIN_BTN_X))
	{
		g_nRun = 0;
		PostMessageW(g_hwndFrame, WM_CLOSE, 0, 0);
	}
}

// ---- frame window ------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_ERASEBKGND:
			return 1; // dcgfx owns the surface
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			EndPaint(hwnd, &ps);
		}
			return 0;
		case WM_SETFOCUS:
			if (g_pHost && g_pHost->GetControlWindow() && !g_nUrlMode)
				SetFocus(g_pHost->GetControlWindow());
			return 0;
		case WM_CLOSE:
			DestroyWindow(hwnd);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
	WNDCLASSW wc;
	MSG msg;
	RECT rc;

	(void)hPrev;
	(void)nShow;
	(void)lpCmd;
	OutputDebugStringW(L"IE: WinMain enter\r\n");

	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
	{
		OutputDebugStringW(L"IE: CoInitializeEx FAILED\r\n");
		return 1;
	}
	LoadLibraryW(L"ieceui.dll"); // register the IE CE controls (per htmlsamp)

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hbrBackground = NULL;
	wc.lpszClassName = L"DCIEXPLORE";
	RegisterClassW(&wc);
	g_hwndFrame = CreateWindowExW(0, L"DCIEXPLORE", L"Internet Explorer",
	                              WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, SCRW, SCRH, NULL,
	                              NULL, hInst, NULL);
	if (!g_hwndFrame)
	{
		CoUninitialize();
		return 1;
	}

	if (!GfxInit(g_hwndFrame)) // take the display via our compositor
	{
		OutputDebugStringW(L"IE: GfxInit FAILED\r\n");
		CoUninitialize();
		return 1;
	}
	GfxInitPageLayer(); // wrap the GWES framebuffer for the page quad

	rc.left = 0;
	rc.top = TOPH;
	rc.right = SCRW;
	rc.bottom = SCRH - BOTH;
	g_pHost = new CBrowserHost(g_hwndFrame, &rc);
	if (!g_pHost->Create())
	{
		OutputDebugStringW(L"IE: host Create FAILED\r\n");
		lstrcpyW(g_szStatus, L"Failed to create the browser control");
	}
	else
	{
		g_pHost->Navigate(HOME_URL);
		if (g_pHost->GetControlWindow())
		{
			ShowWindow(g_pHost->GetControlWindow(), SW_SHOW);
			InvalidateRect(g_pHost->GetControlWindow(), NULL, TRUE);
			UpdateWindow(g_pHost->GetControlWindow());
		}
	}

	DInInit(g_hwndFrame); // our own DI (shell handed input off)

	while (g_nRun)
	{
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				g_nRun = 0;
				break;
			}
			if (g_pHost && !g_nUrlMode) // let the control handle its own keyboard msgs
			{
				HWND hwndCtl = g_pHost->GetControlWindow();
				if (hwndCtl && (msg.hwnd == hwndCtl || IsChild(hwndCtl, msg.hwnd)) &&
				    msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST &&
				    g_pHost->TranslateAccel(&msg) == S_OK)
					continue;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!g_nRun)
			break;

		DInUpdate();
		if (g_nUrlMode)
			PollUrlMode();
		else
			PollPageMode();
		if (g_pHost)
			g_pHost->PumpPending(); // winsock-load any link the control tried to open

		if (g_nUrlMode)
			RenderKeyboard();
		else
			RenderPage();
		GfxWaitVBlank();
	}

	if (g_pHost)
	{
		g_pHost->Destroy();
		g_pHost->Release();
		g_pHost = NULL;
	}
	DInShutdown();
	GfxShutdown();
	CoUninitialize();
	OutputDebugStringW(L"IE: exit\r\n");
	return 0;
}
