//
// dcwtask.c - Task Manager: a DCWin client (own process) that lists the real CE
// processes via toolhelp, shows free RAM, and ends a selected task.
//
// Process enumeration uses toolhelp.dll (dynamically loaded so a missing DLL
// degrades gracefully instead of failing to start). GlobalMemoryStatus and
// OpenProcess/TerminateProcess are coredll APIs; the kill path is SEH-guarded.
//
// Keys (forwarded by the shell to the focused window): Up/Down select,
// Delete or Enter = end task, Esc = close (shell handles it).
//
#include "dcwlib.h"
#include <tlhelp32.h>

#define CW    300
#define CH    222
#define ROWH  14
#define LISTY 40
#define ROWS  11 // visible process rows
#define MAXP  48

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE 0x0001
#endif

typedef HANDLE(WINAPI *PFN_Snap)(DWORD, DWORD);
typedef BOOL(WINAPI *PFN_P32)(HANDLE, LPPROCESSENTRY32);
typedef BOOL(WINAPI *PFN_Close)(HANDLE);
typedef BOOL(WINAPI *PFN_HL)(HANDLE, LPHEAPLIST32);
typedef BOOL(WINAPI *PFN_HF)(HANDLE, LPHEAPENTRY32, DWORD, DWORD);
typedef BOOL(WINAPI *PFN_HN)(HANDLE, LPHEAPENTRY32);

static PFN_Snap g_pfnSnap;
static PFN_P32 g_pfnFirst, g_pfnNext;
static PFN_Close g_pfnClose;
static PFN_HL g_pfnHeapListFirst, g_pfnHeapListNext;
static PFN_HF g_pfnHeapFirst;
static PFN_HN g_pfnHeapNext;

static DWORD g_adwPid[MAXP];
static WCHAR g_awchName[MAXP][32];
static DWORD g_adwMem[MAXP]; // heap bytes per process (0 = n/a)
static int g_n, g_nSel, g_nTop;
static int g_nDrawnOnce = 0; // publish the first frame even if nothing "changed"
static int g_nMemScan = 999; // incremental mem-walk cursor over visible rows (>=ROWS = done)
static WCHAR g_awchStatus[44] = L"";

// Right-click (VK_APPS from the shell) context menu.
#define MENU_N  3
#define MENU_W  108
#define MENU_RH 16
static int g_bMenu = 0; // context menu open
static int g_nMenuX, g_nMenuY, g_nMenuSel;
static const WCHAR *const s_aMenu[MENU_N] = {L"End Task", L"Refresh", L"Crash (test)"};

static const WCHAR *Base(const WCHAR *psz)
{
	const WCHAR *pszB = psz;
	for (; *psz; psz++)
		if (*psz == L'\\')
			pszB = psz + 1;
	return pszB;
}

static int IEq(const WCHAR *pszA, const WCHAR *pszB)
{
	for (; *pszA && *pszB; pszA++, pszB++)
		if ((*pszA | 32) != (*pszB | 32))
			return 0;
	return *pszA == *pszB;
}

// Critical processes we refuse to terminate (killing them freezes the system).
static int Protected(const WCHAR *pszName)
{
	return IEq(pszName, L"nk.exe") || IEq(pszName, L"gwes.exe") || IEq(pszName, L"filesys.exe") ||
	       IEq(pszName, L"device.exe") || IEq(pszName, L"dcshell.exe");
}

static void LoadToolhelp(void)
{
	HINSTANCE h = LoadLibraryW(L"toolhelp.dll");
	if (!h)
	{
		OutputDebugStringW(L"DCWTASK: toolhelp.dll load failed\r\n");
		return;
	}
	g_pfnSnap = (PFN_Snap)GetProcAddress(h, L"CreateToolhelp32Snapshot");
	g_pfnFirst = (PFN_P32)GetProcAddress(h, L"Process32First");
	g_pfnNext = (PFN_P32)GetProcAddress(h, L"Process32Next");
	g_pfnClose = (PFN_Close)GetProcAddress(h, L"CloseToolhelp32Snapshot");
	g_pfnHeapListFirst = (PFN_HL)GetProcAddress(h, L"Heap32ListFirst");
	g_pfnHeapListNext = (PFN_HL)GetProcAddress(h, L"Heap32ListNext");
	g_pfnHeapFirst = (PFN_HF)GetProcAddress(h, L"Heap32First");
	g_pfnHeapNext = (PFN_HN)GetProcAddress(h, L"Heap32Next");
	if (!g_pfnSnap || !g_pfnFirst || !g_pfnNext)
		OutputDebugStringW(L"DCWTASK: toolhelp procs missing\r\n");
}

// Sum the non-free heap blocks of one process = a usable per-process memory
// estimate (CE 2.12 has no working-set API). Heavy, so call only for the selected
// row. SEH-guarded in case this toolhelp build doesn't implement heap walking.
static DWORD MemUsage(DWORD dwPid)
{
	DWORD dwTotal = 0;
	if (!g_pfnSnap || !g_pfnHeapListFirst || !g_pfnHeapFirst || !g_pfnHeapNext)
		return 0;
	__try
	{
		HANDLE hSnap = g_pfnSnap(TH32CS_SNAPHEAPLIST, dwPid);
		HEAPLIST32 hl;
		HEAPENTRY32 he;
		if (!hSnap || hSnap == INVALID_HANDLE_VALUE)
			return 0;
		memset(&hl, 0, sizeof(hl));
		hl.dwSize = sizeof(hl);
		if (g_pfnHeapListFirst(hSnap, &hl))
		{
			do
			{
				memset(&he, 0, sizeof(he));
				he.dwSize = sizeof(he);
				if (g_pfnHeapFirst(hSnap, &he, hl.th32ProcessID, hl.th32HeapID))
					do
					{
						if (!(he.dwFlags & LF32_FREE))
							dwTotal += he.dwBlockSize;
					} while (g_pfnHeapNext(hSnap, &he));
			} while (g_pfnHeapListNext && g_pfnHeapListNext(hSnap, &hl));
		}
		if (g_pfnClose)
			g_pfnClose(hSnap);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		dwTotal = 0;
	}
	return dwTotal;
}

static void Scan(void)
{
	HANDLE hSnap;
	PROCESSENTRY32 pe;
	g_n = 0;
	if (!g_pfnSnap || !g_pfnFirst || !g_pfnNext)
		return;
	hSnap = g_pfnSnap(TH32CS_SNAPPROCESS, 0);
	if (!hSnap || hSnap == INVALID_HANDLE_VALUE)
		return;
	memset(&pe, 0, sizeof(pe));
	pe.dwSize = sizeof(pe);
	if (g_pfnFirst(hSnap, &pe))
	{
		do
		{
			const WCHAR *pszNm = Base(pe.szExeFile);
			int k;
			if (g_n >= MAXP)
				break;
			for (k = 0; k < 31 && pszNm[k]; k++)
				g_awchName[g_n][k] = pszNm[k];
			g_awchName[g_n][k] = 0;
			g_adwPid[g_n] = pe.th32ProcessID;
			g_n++;
		} while (g_pfnNext(hSnap, &pe));
	}
	if (g_pfnClose)
		g_pfnClose(hSnap);
	if (g_nSel >= g_n)
		g_nSel = g_n ? g_n - 1 : 0;
	g_nMemScan = 0; // kick a fresh (incremental) per-process mem pass over visible rows
}

static void EndTask(void)
{
	if (g_nSel < 0 || g_nSel >= g_n)
		return;
	if (Protected(g_awchName[g_nSel]))
	{
		lstrcpyW(g_awchStatus, L"protected - won't end");
		return;
	}
	__try
	{
		HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, g_adwPid[g_nSel]);
		if (h)
		{
			TerminateProcess(h, 0);
			CloseHandle(h);
			wsprintfW(g_awchStatus, L"ended %s", g_awchName[g_nSel]);
		}
		else
			lstrcpyW(g_awchStatus, L"can't open process");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		lstrcpyW(g_awchStatus, L"end-task unavailable");
	}
}

static void RamLine(WCHAR *pszOut)
{
	MEMORYSTATUS ms;
	memset(&ms, 0, sizeof(ms));
	ms.dwLength = sizeof(ms);
	__try
	{
		GlobalMemoryStatus(&ms);
		wsprintfW(pszOut, L"RAM  %u K free / %u K  (%u%% used)", (unsigned)(ms.dwAvailPhys / 1024),
		          (unsigned)(ms.dwTotalPhys / 1024), (unsigned)ms.dwMemoryLoad);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		lstrcpyW(pszOut, L"RAM  (unavailable)");
	}
}

// "crash" anywhere in the command line (case-insensitive). Robust against CE passing lpCmd as
// "crash" OR "dcwtask.exe crash" OR with leading whitespace.
static int HasCrashArg(const WCHAR *psz)
{
	int i;
	if (!psz)
		return 0;
	for (i = 0; psz[i]; i++)
		if ((psz[i] | 32) == 'c' && (psz[i + 1] | 32) == 'r' && (psz[i + 2] | 32) == 'a' &&
		    (psz[i + 3] | 32) == 's' && (psz[i + 4] | 32) == 'h')
			return 1;
	return 0;
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *w;
	DWORD dwKey, dwNextScan;

	{
		WCHAR achDbg[96];
		wsprintfW(achDbg, L"DCWTASK: lpCmd=[%s]\r\n", lpCmd ? lpCmd : L"(null)");
		OutputDebugStringW(achDbg);
	}
	// "dcwtask.exe crash" (Start menu -> Crash Test) raises an unhandled exception so dcwlib's SEH
	// wrapper reports it and the shell blue-screens. RaiseException is reliably caught (a wild
	// pointer may not fault on every CE config); the 2 args fake a realistic access violation.
	if (HasCrashArg(lpCmd))
	{
		DWORD a[2] = {1 /* write */, 0xBADC0DE};
		OutputDebugStringW(L"DCWTASK: raising crash exception\r\n");
		RaiseException(STATUS_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, a);
	}

	w = DCWinOpen(150, 70, CW, CH, L"Task Manager", ICON_APP);
	if (!w)
	{
		OutputDebugStringW(L"DCWTASK: DCWinOpen failed\r\n");
		return 1;
	}

	LoadToolhelp();
	Scan();
	dwNextScan = GetTickCount() + 1000;

	for (;;)
	{
		WCHAR awchRam[64], awchRow[64], awchMem[12];
		int i, y, nChanged = 0, nCw = CW, nCh = CH;
		nChanged |= DCWinClientSize(w, &nCw, &nCh); // resize/maximize -> redraw to fit

		// Pointer hover: highlight the process row (or menu item) under the cursor. The shell
		// synthesizes VK_RETURN on a body click, so opening it is handled in the key loop below.
		{
			int px = 0, py = 0, btn = 0;
			if (DCWinGetPointer(w, &px, &py, &btn))
			{
				if (g_bMenu)
				{
					if (px >= g_nMenuX && px < g_nMenuX + MENU_W && py >= g_nMenuY + 2 &&
					    py < g_nMenuY + 2 + MENU_N * MENU_RH)
					{
						int m = (py - (g_nMenuY + 2)) / MENU_RH;
						if (m != g_nMenuSel)
						{
							g_nMenuSel = m;
							nChanged = 1;
						}
					}
				}
				else if (py >= LISTY)
				{
					int row = g_nTop + (py - LISTY) / ROWH;
					if (row >= 0 && row < g_n && row < g_nTop + ROWS && row != g_nSel)
					{
						g_nSel = row;
						nChanged = 1;
					}
				}
			}
		}
		while (DCWinPollKey(w, &dwKey))
		{
			int nOtop = g_nTop;
			if (g_bMenu) // context menu captures keys
			{
				if (dwKey == VK_UP && g_nMenuSel > 0)
					g_nMenuSel--;
				else if (dwKey == VK_DOWN && g_nMenuSel < MENU_N - 1)
					g_nMenuSel++;
				else if (dwKey == VK_APPS) // right-click again: dismiss
					g_bMenu = 0;
				else if (dwKey == VK_RETURN) // click/Enter: run the item, unless clicked off-menu
				{
					int px = 0, py = 0, btn = 0, bOff = 0;
					if (DCWinGetPointer(w, &px, &py, &btn))
						bOff = !(px >= g_nMenuX && px < g_nMenuX + MENU_W && py >= g_nMenuY &&
						         py < g_nMenuY + 2 + MENU_N * MENU_RH);
					g_bMenu = 0;
					if (!bOff && g_nMenuSel == 0)
						EndTask();
					else if (!bOff && g_nMenuSel == 2) // exercises the shell's BSOD
					{
						DWORD a[2] = {1, 0xBADC0DE};
						RaiseException(STATUS_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, a);
					}
					Scan();
				}
				nChanged = 1;
				continue;
			}
			if (dwKey == VK_UP && g_nSel > 0)
				g_nSel--;
			else if (dwKey == VK_DOWN && g_nSel < g_n - 1)
				g_nSel++;
			else if (dwKey == VK_DELETE || dwKey == VK_RETURN)
			{
				EndTask();
				Scan();
			}
			else if (dwKey == VK_APPS) // right-click -> open the context menu at the cursor
			{
				int px = CW / 2, py = LISTY, btn = 0;
				DCWinGetPointer(w, &px, &py, &btn);
				if (px > CW - MENU_W)
					px = CW - MENU_W;
				if (py > CH - (MENU_N * MENU_RH + 6))
					py = CH - (MENU_N * MENU_RH + 6);
				if (px < 0)
					px = 0;
				if (py < LISTY)
					py = LISTY;
				g_nMenuX = px;
				g_nMenuY = py;
				g_nMenuSel = 0;
				g_bMenu = 1;
			}
			if (g_nSel < g_nTop)
				g_nTop = g_nSel;
			if (g_nSel >= g_nTop + ROWS)
				g_nTop = g_nSel - ROWS + 1;
			if (g_nTop != nOtop)
				g_nMemScan = 0; // scrolled: refresh visible rows' mem
			nChanged = 1;
		}
		if (GetTickCount() >= dwNextScan)
		{
			Scan();
			dwNextScan += 1000;
			nChanged = 1;
		}

		// Per-process memory is an expensive toolhelp heap-walk, so do it ONE row
		// per loop over just the visible rows (after a scan/scroll), never all at
		// once - otherwise it hitches ~1x/s and stalls input. Redraw once the
		// visible pass completes.
		if (g_nMemScan < ROWS)
		{
			int nIdx = g_nTop + g_nMemScan;
			if (nIdx < g_n)
				g_adwMem[nIdx] = MemUsage(g_adwPid[nIdx]);
			if (++g_nMemScan >= ROWS)
				nChanged = 1;
		}

		// Publish a frame ONLY when something changed (key, scan, status). Re-
		// publishing every loop bumps our gen and forces the shell to recomposite
		// the whole desktop 16x/s just because this window is open - the lag.
		if (nChanged || !g_nDrawnOnce)
		{
			g_nDrawnOnce = 1;
			DCWinBeginFrame(w);
			DCWinFillBg(w, RGB(192, 192, 192)); // background fills the window
			RamLine(awchRam);
			DCWinText(w, 8, 6, RGB(0, 0, 0), RGB(192, 192, 192), awchRam);
			DCWinFill(w, 6, 22, nCw - 12, ROWH, RGB(0, 0, 128)); // header spans the width
			DCWinText(w, 10, 23, RGB(255, 255, 255), RGB(0, 0, 128), L"PID    Mem    Image");

			for (i = 0; i < ROWS && g_nTop + i < g_n; i++)
			{
				int nIdx = g_nTop + i;
				COLORREF bg = (nIdx == g_nSel) ? RGB(0, 0, 160) : RGB(192, 192, 192);
				COLORREF fg = (nIdx == g_nSel) ? RGB(255, 255, 255) : RGB(0, 0, 0);
				y = LISTY + i * ROWH;
				if (nIdx == g_nSel)
					DCWinFill(w, 6, y, CW - 12, ROWH, bg);
				if (g_adwMem[nIdx])
					wsprintfW(awchMem, L"%5uK", (unsigned)(g_adwMem[nIdx] / 1024));
				else
					lstrcpyW(awchMem, L"   --");
				wsprintfW(awchRow, L"%5u %s %s", (unsigned)g_adwPid[nIdx], awchMem,
				          g_awchName[nIdx]);
				DCWinText(w, 10, y + 1, fg, bg, awchRow);
			}

			y = LISTY + ROWS * ROWH + 4;
			wsprintfW(awchRow, L"%d procs   Del/Enter: end task", g_n);
			DCWinText(w, 8, y, RGB(0, 0, 0), RGB(192, 192, 192), awchRow);
			if (g_awchStatus[0])
				DCWinText(w, 8, y + ROWH, RGB(128, 0, 0), RGB(192, 192, 192), g_awchStatus);
			if (g_bMenu) // context menu popup, on top of the list
			{
				int mi, h = MENU_N * MENU_RH + 4;
				DCWinFill(w, g_nMenuX, g_nMenuY, MENU_W, h, RGB(192, 192, 192));
				DCWinFill(w, g_nMenuX, g_nMenuY, MENU_W, 1, RGB(64, 64, 64));
				DCWinFill(w, g_nMenuX, g_nMenuY + h - 1, MENU_W, 1, RGB(64, 64, 64));
				DCWinFill(w, g_nMenuX, g_nMenuY, 1, h, RGB(64, 64, 64));
				DCWinFill(w, g_nMenuX + MENU_W - 1, g_nMenuY, 1, h, RGB(64, 64, 64));
				for (mi = 0; mi < MENU_N; mi++)
				{
					int my = g_nMenuY + 2 + mi * MENU_RH;
					COLORREF bg = (mi == g_nMenuSel) ? RGB(0, 0, 128) : RGB(192, 192, 192);
					COLORREF fg = (mi == g_nMenuSel) ? RGB(255, 255, 255) : RGB(0, 0, 0);
					if (mi == g_nMenuSel)
						DCWinFill(w, g_nMenuX + 2, my, MENU_W - 4, MENU_RH, bg);
					DCWinText(w, g_nMenuX + 6, my + 1, fg, bg, s_aMenu[mi]);
				}
			}
			DCWinEndFrame(w);
		}

		if (DCWinShouldClose(w))
			break;
		Sleep(20);
	}

	DCWinClose(w);
	return 0;
}
