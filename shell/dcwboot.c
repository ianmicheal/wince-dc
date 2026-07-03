//
// dcwboot.c - the DCWin boot loader. Baked as HKLM\init Autorun, it runs BEFORE the desktop and
// shows an NT-style "select an option" menu (drawn with the same Direct3D/PVR2 path dcshell uses -
// dcgfx fills + atlas text; input via dcinput, controller D-pad / A or the keyboard):
//
//     Boot DCWin (Normal Mode)            -> CreateProcess dcshell.exe (this image)
//     Boot DCWin with Internet Explorer   -> chainload 0WINCEOSHTML.BIN (a 2nd CE image, IE baked)
//
// The IE image is a separate, bigger ROM (the Trident/IE stack bloats it ~6 MB), so it can't be
// vendored in the same image - the loader reads it off the disc and chainloads it. (Phase 1: the
// chainloader itself is not wired yet; the option detects the image + reports if it's absent.)
// The network stack is kicked in the background so DHCP is up by the time the desktop loads.
//
#include <windows.h>
#include <winsock.h>
#include "dcgfx.h"   // GfxInit/GfxFill/GfxText/GfxPresent + g_Font* + SCREEN_*
#include "dcinput.h" // DInInit/DInUpdate/DInNextKey (controller + keyboard)

#define C_BG       RGB(0, 0, 0)       // NT loader: black background
#define C_TEXT     RGB(200, 200, 200) // dim white body text
#define C_HI_BG    RGB(192, 192, 192) // selected row: light-grey bar
#define C_HI_FG    RGB(0, 0, 0)       // selected row: black text
#define C_TITLE    RGB(255, 255, 255) // bright white headings
#define C_MUTE     RGB(120, 120, 120)
#define C_OK       RGB(93, 202, 165)
#define C_FAIL     RGB(226, 75, 74)

#define HTML_IMAGE L"\\CD-ROM\\0WINCEOSHTML.BIN"

// The chainloader hand-off. Chainload() (trampoline.src) ORs srMask into SR, scatter-gather copies
// the payload to dst, and jumps to entry; it never returns. CRITICAL: the copy must touch nothing
// translated. The running kernel's exception vectors AND dcwboot itself are XIP from the NK region
// that the copy overwrites; the source comes from VirtualAlloc, whose pages are translated P0 AND
// physically scattered. So we (1) LockPages the source to learn each page's PHYSICAL address and
// build a page-frame list the trampoline indexes, reading every source page through its P2
// untranslated alias, and (2) relocate the trampoline's code + descriptor into a separate RAM-region
// buffer run through ITS P2 alias. Then no TLB is ever consulted and SR.BL is safe. SetKMode,
// LockPages, and CacheSync all live in coredll.
typedef struct
{
	DWORD dst;        // P2 alias of the load base, 0xAC010000 (uncached -> writes straight to RAM)
	DWORD nPages;     // number of 4 KB source pages to copy
	DWORD entry;      // 0x8C010004 - the new kernel's entry (P1; untranslated, no TLB at the jump)
	DWORD srMask;     // bits OR'd into SR before the copy (BL|IMASK - safe: all accesses untranslated)
	DWORD aPfnPg[8];  // P2 aliases of the one-page buffers holding the raw physical page addresses
	                  // (1024 entries each); the trampoline reads source page i's PA from
	                  // aPfnPg[i>>10][i&1023]. 8 pages -> up to 32 MB of payload.
} TrampParams;
extern void Chainload(TrampParams *p); // the trampoline body (copied out + run via its P2 alias)
extern void ChainloadEnd(void);        // marker just past it, to measure the code length

extern DWORD SetKMode(DWORD fMode);
extern DWORD LockPages(LPVOID pv, DWORD cb, PDWORD pPFNs, int fFlags);
extern void CacheSync(DWORD dwFlags);

#define LOCKFLAG_QUERY_ONLY 0x01
#define LOCKFLAG_READ 0x02
#define CHAIN_PAGE 0x1000   // SH-4 CE page size
#define CHAIN_MAXPFNPG 8    // max pfn pages (matches TrampParams.aPfnPg) -> 32 MB payload ceiling
// CacheSync flags: write back + discard the D-cache (push the file/descriptor/pfn bytes to RAM) and
// invalidate the I-cache (the dest is about to hold new code).
#define CHAIN_CACHE (0x01 /*DISCARD*/ | 0x02 /*INSTRUCTIONS*/ | 0x04 /*WRITEBACK*/)

// CE may report LockPages page-frames as raw byte addresses or as PA>>8 / PA>>12. Detect the shift
// that lands pfn0 inside the known DC RAM window [0x0C600000,0x0D000000); 0xFFFFFFFF = unrecognised.
static DWORD ShiftOf(DWORD pfn0)
{
	static const int aShift[3] = {0, 8, 12};
	int j;
	for (j = 0; j < 3; j++)
	{
		DWORD cand = pfn0 << aShift[j];
		if (cand >= 0x0C600000 && cand < 0x0D000000)
			return (DWORD)aShift[j];
	}
	return 0xFFFFFFFF;
}

// Physical byte address of a single-page buffer's page (0 on failure). Used for the trampoline page
// and the pfn pages - all one page, so contiguity is moot.
static DWORD PagePhys(void *pv)
{
	DWORD pfn = 0, shift;
	if (!LockPages(pv, CHAIN_PAGE, &pfn, LOCKFLAG_QUERY_ONLY | LOCKFLAG_READ))
		return 0;
	shift = ShiftOf(pfn);
	if (shift == 0xFFFFFFFF)
		return 0;
	return pfn << shift;
}

// P2 (uncached, untranslated) alias of a physical byte address.
static DWORD P2(DWORD pa)
{
	return (pa & 0x1FFFFFFF) | 0xA0000000;
}

static int s_nSel = 0; // 0 = Normal, 1 = Internet Explorer
static int s_bNetKicked = 0;
static WCHAR s_szMsg[64] = L""; // transient status line under the menu

static const WCHAR *s_aOpt[2] = {
    L"Boot DCWin (Normal Mode)",
    L"Boot DCWin with Internet Explorer",
};
#define NOPT 2

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

static void LaunchShell(void) // hand the display off + run the desktop
{
	PROCESS_INFORMATION pi;
	GfxShutdown();
	if (!CreateProcessW(L"\\Windows\\dcshell.exe", L"", 0, 0, 0, 0, 0, 0, 0, &pi))
		CreateProcessW(L"dcshell.exe", L"", 0, 0, 0, 0, 0, 0, 0, &pi);
}

static DWORD Rd32(const BYTE *p) // little-endian, alignment-safe
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

// Boot the Internet Explorer image. It's a separate, bigger CE ROM on the disc (the Trident/IE
// stack bloats it ~6 MB), so it can't be vendored in the same image - the loader reads the whole
// .BIN into RAM and chainloads it. Everything the copy touches is funnelled through untranslated P2
// aliases (see the TrampParams comment): the source via its LockPages'd physical pages, the dest at
// 0xAC010000, and the trampoline code itself (relocated out of the NK region it's about to clobber).
// The new kernel cold-boots itself, so the trampoline just copies + jumps. Never returns; on any
// failure before the jump we drop back to the menu with a status line.
static int BootIeImage(void)
{
	HANDLE hf = INVALID_HANDLE_VALUE;
	BYTE *pBuf = NULL, *pTramp = NULL;
	BYTE *apfnPg[CHAIN_MAXPFNPG];
	DWORD *pfnTmp = NULL;
	TrampParams *pDesc;
	BYTE hdr[0x24];
	DWORD dwRead = 0, dwBase, dwOff, dwLen, nPages, nPfnPg, cbCode, descOff, shift, i, k;
	DWORD paTramp, codeP2, descP2;
	WCHAR b[192];

	for (k = 0; k < CHAIN_MAXPFNPG; k++)
		apfnPg[k] = NULL;

	if (GetFileAttributesW(HTML_IMAGE) == 0xFFFFFFFF)
	{
		lstrcpyW(s_szMsg, L"IE image not on disc - rebuild with -DHTML=on");
		return 0;
	}
	hf = CreateFileW(HTML_IMAGE, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                 FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE)
	{
		lstrcpyW(s_szMsg, L"IE image open failed");
		return 0;
	}
	if (!ReadFile(hf, hdr, sizeof(hdr), &dwRead, NULL) || dwRead < sizeof(hdr) || hdr[0] != 0xD6 ||
	    hdr[1] != 0x1A)
	{
		lstrcpyW(s_szMsg, L"IE image bad D61A header");
		goto fail;
	}
	dwBase = Rd32(hdr + 0x14); // physical load base (0x0C010000)
	dwOff = Rd32(hdr + 0x18);  // payload offset (0x800)
	dwLen = Rd32(hdr + 0x1C);  // payload length (padded, page-multiple)

	nPages = (dwLen + CHAIN_PAGE - 1) / CHAIN_PAGE; // 4 KB pages of payload
	nPfnPg = (nPages + 1023) / 1024;               // one-page pfn buffers needed
	if (nPfnPg > CHAIN_MAXPFNPG)
	{
		lstrcpyW(s_szMsg, L"IE image too large to chainload");
		goto fail;
	}

	// Payload buffer (VirtualAlloc is page-aligned, matching the page-aligned load base) + a scratch
	// array for the raw PFN list (read in C, so it may be translated/scattered) + a one-page buffer
	// for the relocated trampoline code + descriptor.
	pBuf = (BYTE *)VirtualAlloc(NULL, dwLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	pfnTmp = (DWORD *)VirtualAlloc(NULL, (nPages + 4) * 4, MEM_COMMIT, PAGE_READWRITE);
	pTramp = (BYTE *)VirtualAlloc(NULL, CHAIN_PAGE, MEM_COMMIT, PAGE_READWRITE);
	if (!pBuf || !pfnTmp || !pTramp)
	{
		lstrcpyW(s_szMsg, L"IE chainload: out of RAM");
		goto fail;
	}

	// Read the payload (skip the D61A header) straight onto the page-aligned buffer, so source page i
	// maps to dest page i with no intra-page offset.
	SetFilePointer(hf, dwOff, NULL, FILE_BEGIN);
	if (!ReadFile(hf, pBuf, dwLen, &dwRead, NULL) || dwRead < dwLen)
	{
		lstrcpyW(s_szMsg, L"IE image: payload read failed");
		goto fail;
	}
	CloseHandle(hf);
	hf = INVALID_HANDLE_VALUE;

	// Relocate the trampoline body into pTramp; lay its descriptor 32-byte-aligned just past it.
	cbCode = (DWORD)(BYTE *)ChainloadEnd - (DWORD)(BYTE *)Chainload;
	memcpy(pTramp, (void *)Chainload, cbCode);
	descOff = (cbCode + 31) & ~31u;
	pDesc = (TrampParams *)(pTramp + descOff);

	// Learn every source page's physical address.
	if (!LockPages(pBuf, dwLen, pfnTmp, LOCKFLAG_QUERY_ONLY | LOCKFLAG_READ))
	{
		lstrcpyW(s_szMsg, L"IE chainload: LockPages failed");
		goto fail;
	}
	shift = ShiftOf(pfnTmp[0]);
	if (shift == 0xFFFFFFFF)
	{
		wsprintfW(b, L"DCWBOOT: bad pfn encoding pfn0=%08X\r\n", (unsigned)pfnTmp[0]);
		OutputDebugStringW(b);
		lstrcpyW(s_szMsg, L"IE chainload: bad pfn encoding (see log)");
		goto fail;
	}

	// Split the page-frame list into one-page buffers (each inherently contiguous), storing each
	// source page's *physical byte address*; record every pfn page's P2 alias in the descriptor.
	for (k = 0; k < nPfnPg; k++)
	{
		DWORD cnt = nPages - k * 1024, pa;
		if (cnt > 1024)
			cnt = 1024;
		apfnPg[k] = (BYTE *)VirtualAlloc(NULL, CHAIN_PAGE, MEM_COMMIT, PAGE_READWRITE);
		if (!apfnPg[k])
		{
			lstrcpyW(s_szMsg, L"IE chainload: pfn page alloc failed");
			goto fail;
		}
		for (i = 0; i < cnt; i++)
			((DWORD *)apfnPg[k])[i] = pfnTmp[k * 1024 + i] << shift;
		pa = PagePhys(apfnPg[k]);
		if (!pa)
		{
			lstrcpyW(s_szMsg, L"IE chainload: pfn page phys failed");
			goto fail;
		}
		pDesc->aPfnPg[k] = P2(pa);
	}

	paTramp = PagePhys(pTramp);
	if (!paTramp)
	{
		lstrcpyW(s_szMsg, L"IE chainload: trampoline phys failed");
		goto fail;
	}
	codeP2 = P2(paTramp);
	descP2 = P2(paTramp + descOff);

	pDesc->dst = P2(dwBase);                                  // 0xAC010000
	pDesc->nPages = nPages;
	pDesc->entry = ((dwBase & 0x1FFFFFFF) | 0x80000000) + 4;  // 0x8C010004 (P1, untranslated)
	pDesc->srMask = 0x100000F0;                               // SR.BL | IMASK=0xF (all accesses P2)

	wsprintfW(b,
	          L"DCWBOOT: chain dst=%08X nPg=%X entry=%08X codeP2=%08X descP2=%08X pfnPg=%u cb=%X\r\n",
	          (unsigned)pDesc->dst, (unsigned)nPages, (unsigned)pDesc->entry, (unsigned)codeP2,
	          (unsigned)descP2, (unsigned)nPfnPg, (unsigned)cbCode);
	OutputDebugStringW(b);

	GfxShutdown();          // release the display object before the kernel goes away
	SetKMode(TRUE);         // privileged: the trampoline touches SR + raw P2
	CacheSync(CHAIN_CACHE); // push payload + trampoline + descriptor + pfn pages to RAM; drop I-cache
	((void (*)(TrampParams *))codeP2)((TrampParams *)descP2); // copies + jumps - never returns

	// Only reached if the hand-off somehow fell through.
	lstrcpyW(s_szMsg, L"IE image: chainload returned?!");
	return 0;

fail:
	if (hf != INVALID_HANDLE_VALUE)
		CloseHandle(hf);
	if (pBuf)
		VirtualFree(pBuf, 0, MEM_RELEASE);
	if (pfnTmp)
		VirtualFree(pfnTmp, 0, MEM_RELEASE);
	if (pTramp)
		VirtualFree(pTramp, 0, MEM_RELEASE);
	for (k = 0; k < CHAIN_MAXPFNPG; k++)
		if (apfnPg[k])
			VirtualFree(apfnPg[k], 0, MEM_RELEASE);
	return 0;
}

static void Render(DWORD t)
{
	HDC hdc;
	int i, x = 40, y0 = 120, rowH = 22, nMidX = SCREEN_W / 2;
	(void)t;

	GfxFill(0, 0, SCREEN_W, SCREEN_H, C_BG);
	// selected-row light-grey bar (drawn in the fills pass, before the text)
	GfxFill(x - 6, y0 + s_nSel * rowH - 2, x + 380, y0 + s_nSel * rowH + 18, C_HI_BG);

	hdc = GfxLockDC();
	GfxText(hdc, x, 30, C_TITLE, C_BG, g_FontBold, L"DCWin Loader");
	GfxText(hdc, x, 60, C_TEXT, C_BG, g_FontUI, L"Dreamcast Community Edition - select an option:");
	for (i = 0; i < NOPT; i++)
	{
		COLORREF fg = (i == s_nSel) ? C_HI_FG : C_TEXT;
		COLORREF bg = (i == s_nSel) ? C_HI_BG : C_BG;
		GfxText(hdc, x, y0 + i * rowH, fg, bg, g_FontUI, s_aOpt[i]);
	}
	GfxText(hdc, x, y0 + NOPT * rowH + 30, C_MUTE, C_BG, g_FontUI,
	        L"Use Up and Down to move the highlight to your choice.");
	GfxText(hdc, x, y0 + NOPT * rowH + 52, C_MUTE, C_BG, g_FontUI, L"Press A or Enter to choose.");
	if (s_szMsg[0])
		GfxText(hdc, x, y0 + NOPT * rowH + 90, C_FAIL, C_BG, g_FontUI, s_szMsg);
	GfxText(hdc, x, SCREEN_H - 28, C_MUTE, C_BG, g_FontUI,
	        s_bNetKicked ? L"SH-4 - network starting..." : L"SH-4 - retail");
	GfxUnlockDC(hdc);
	(void)nMidX;
}

static void Choose(int *pbDone) // run the highlighted option (A / Enter)
{
	s_szMsg[0] = 0;
	if (s_nSel == 0)
	{
		LaunchShell();
		*pbDone = 1;
	}
	else
		BootIeImage(); // stays in the menu until the chainloader lands
}

int DcwBootMain(void)
{
	int bDone = 0;
	DWORD dwVk;

	DInInit(NULL); // controller + keyboard (no window needed for DInput)
	KickNetwork(); // bring the network up in the background while the menu is shown

	while (!bDone)
	{
		DInUpdate();
		// D-pad (queued VKs) + keyboard Enter; the controller A button is a separate "activate"
		// edge (DInTookActivate), not a queued key - handle it explicitly below.
		while (DInNextKey(&dwVk))
		{
			s_szMsg[0] = 0;
			if (dwVk == VK_UP)
				s_nSel = (s_nSel + NOPT - 1) % NOPT;
			else if (dwVk == VK_DOWN)
				s_nSel = (s_nSel + 1) % NOPT;
			else if (dwVk == VK_RETURN)
				Choose(&bDone);
		}
		if (DInTookActivate()) // controller A
			Choose(&bDone);
		Render(GetTickCount());
		GfxPresent(0, 0, FALSE); // no cursor on the boot menu
		GfxWaitVBlank();
	}
	return 0;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
	WNDCLASSW wc;
	HWND hWnd;
	(void)hPrev;
	(void)lpCmd;
	(void)nShow;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hbrBackground = NULL;
	wc.lpszClassName = L"DCWBOOT";
	RegisterClassW(&wc);
	hWnd = CreateWindowExW(0, L"DCWBOOT", L"DCWin", WS_VISIBLE, 0, 0, SCREEN_W, SCREEN_H, NULL,
	                       NULL, hInst, NULL);
	if (hWnd && GfxInit(hWnd))
		DcwBootMain();
	else
		LaunchShell(); // no display -> just boot the desktop
	return 0;
}
