//
// dcwlib.c - DCWin client library (see dcwlib.h). Maps the shared section, claims
// a window slot, and marshals draw commands / input. One window per client.
//
#include "dcwlib.h"

struct DCWin
{
	HANDLE hMap;
	DcShared *sh;
	DcWindow *w;
	DcCmd build[DCWIN_MAXCMD];
	int buildN;
	int lastW, lastH; // last size seen by DCWinClientSize (change detection)
};

static struct DCWin g_win; // single window per client process

DCWin *DCWinOpen(int x, int y, int w, int h, const WCHAR *title, int iconId)
{
	int i, nIdx = -1, nTries;

	g_win.hMap =
	    CreateFileMappingW((HANDLE)-1, NULL, PAGE_READWRITE, 0, sizeof(DcShared), DCWIN_SECTION);
	if (!g_win.hMap)
		return NULL;
	g_win.sh = (DcShared *)MapViewOfFile(g_win.hMap, FILE_MAP_WRITE, 0, 0, sizeof(DcShared));
	if (!g_win.sh)
		return NULL;

	for (nTries = 0; nTries < 50 && g_win.sh->magic != DCWIN_MAGIC; nTries++)
		Sleep(20); // wait for the shell to initialise the section
	if (g_win.sh->magic != DCWIN_MAGIC)
		return NULL;

	for (i = 0; i < DCWIN_MAXWIN; i++)
		if (!g_win.sh->win[i].inUse)
		{
			nIdx = i;
			break;
		}
	if (nIdx < 0)
		return NULL;

	g_win.w = &g_win.sh->win[nIdx];
	memset(g_win.w, 0, sizeof(DcWindow));
	g_win.w->ownerPid = GetCurrentProcessId();
	g_win.w->x = x;
	g_win.w->y = y;
	g_win.w->w = w;
	g_win.w->h = h;
	lstrcpyW(g_win.w->title, title);
	g_win.w->icon = (DWORD)iconId;
	g_win.w->cmdCount = 0;
	g_win.w->gen = 0; // even = stable (seqlock)
	g_win.buildN = 0;
	g_win.lastW = w;
	g_win.lastH = h;
	g_win.w->inUse = 1; // publish only once fully initialised
	return &g_win;
}

// Current client size (the shell may have resized us). Returns 1 if it changed since last call.
int DCWinClientSize(DCWin *pWin, int *cw, int *ch)
{
	int w = (int)pWin->w->w, h = (int)pWin->w->h, bChanged;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	bChanged = (w != pWin->lastW || h != pWin->lastH);
	pWin->lastW = w;
	pWin->lastH = h;
	if (cw)
		*cw = w;
	if (ch)
		*ch = h;
	return bChanged;
}

// 1 if the shell resized us since last call (consumes the change). OR into your dirty flag.
int DCWinResized(DCWin *pWin)
{
	return DCWinClientSize(pWin, 0, 0);
}

// Fill the whole current client area with one colour - the standard first draw of a frame, so
// the window always fills after a resize/maximize without the app tracking dimensions.
void DCWinFillBg(DCWin *pWin, COLORREF color)
{
	int w = (int)pWin->w->w, h = (int)pWin->w->h;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	DCWinFill(pWin, 0, 0, w, h, color);
}

void DCWinBeginFrame(DCWin *pWin)
{
	pWin->buildN = 0;
}

void DCWinFill(DCWin *pWin, int x, int y, int w, int h, COLORREF color)
{
	DcCmd *pCmd;
	if (pWin->buildN >= DCWIN_MAXCMD)
		return;
	pCmd = &pWin->build[pWin->buildN++];
	pCmd->op = DCOP_FILL;
	pCmd->x = x;
	pCmd->y = y;
	pCmd->w = w;
	pCmd->h = h;
	pCmd->color = color;
}

void DCWinText(DCWin *pWin, int x, int y, COLORREF fg, COLORREF bg, const WCHAR *text)
{
	DcCmd *pCmd;
	int k;
	if (pWin->buildN >= DCWIN_MAXCMD)
		return;
	pCmd = &pWin->build[pWin->buildN++];
	pCmd->op = DCOP_TEXT;
	pCmd->x = x;
	pCmd->y = y;
	pCmd->color = fg;
	pCmd->color2 = bg;
	for (k = 0; k < 39 && text[k]; k++)
		pCmd->text[k] = text[k];
	pCmd->text[k] = 0;
}

void DCWinIcon(DCWin *pWin, int x, int y, int iconId)
{
	DcCmd *pCmd;
	if (pWin->buildN >= DCWIN_MAXCMD)
		return;
	pCmd = &pWin->build[pWin->buildN++];
	pCmd->op = DCOP_ICON;
	pCmd->x = x;
	pCmd->y = y;
	pCmd->color = (DWORD)iconId;
}

void DCWinEndFrame(DCWin *pWin)
{
	int i;
	pWin->w->gen++; // odd = writing (seqlock)
	for (i = 0; i < pWin->buildN; i++)
		pWin->w->cmd[i] = pWin->build[i];
	pWin->w->cmdCount = pWin->buildN;
	pWin->w->gen++; // even = stable
}

int DCWinPollKey(DCWin *pWin, DWORD *key)
{
	if (pWin->w->inTail == pWin->w->inHead)
		return 0;
	*key = pWin->w->in[pWin->w->inTail % DCWIN_MAXIN].key;
	pWin->w->inTail++;
	return 1;
}

int DCWinGetPointer(DCWin *pWin, int *x, int *y, int *btn)
{
	if (pWin->w->ptrX < 0)
		return 0; // analog-stick cursor not over this window
	if (x)
		*x = (int)pWin->w->ptrX;
	if (y)
		*y = (int)pWin->w->ptrY;
	if (btn)
		*btn = (int)pWin->w->ptrBtn;
	return 1;
}

int DCWinShouldClose(DCWin *pWin)
{
	return pWin->w->wantClose != 0;
}

void DCWinExec(DCWin *pWin, const WCHAR *path)
{
	lstrcpyW(pWin->sh->execPath, path);
	pWin->sh->execSeq++; // shell polls execSeq and launches execPath
}

void DCWinClose(DCWin *pWin)
{
	pWin->w->inUse = 0;
	if (pWin->sh)
		UnmapViewOfFile(pWin->sh);
	if (pWin->hMap)
		CloseHandle(pWin->hMap);
}
