//
// syslog.c - see syslog.h. Lazily creates/maps the DCSYSLOG shared section and appends
// lines into its ring. Self-contained (just coredll) so it compiles into any module.
//
#include <stdarg.h>
#include "syslog.h"

static SysLogShared *g_pSl;
static HANDLE g_hMap;

SysLogShared *SysLogMap(int create)
{
	if (g_pSl)
		return g_pSl;
	// CreateFileMapping on the same name returns the one global section (zero-filled on first
	// create), so every process - and a WDM driver in wdevice.exe - shares the same ring.
	g_hMap = CreateFileMappingW((HANDLE)-1, NULL, PAGE_READWRITE, 0, sizeof(SysLogShared),
	                            SYSLOG_SECTION);
	if (!g_hMap)
		return NULL;
	g_pSl = (SysLogShared *)MapViewOfFile(g_hMap, FILE_MAP_WRITE, 0, 0, sizeof(SysLogShared));
	if (g_pSl && create && g_pSl->magic != SYSLOG_MAGIC)
		g_pSl->magic = SYSLOG_MAGIC; // first writer stamps it
	return g_pSl;
}

void SysLogW(const WCHAR *psz)
{
	SysLogShared *pSl = SysLogMap(1);
	LONG lIdx;
	int i;
	WCHAR *pszDst;
	if (!pSl || !psz)
		return;
	lIdx = InterlockedIncrement(&pSl->head) - 1; // claim a slot
	pszDst = pSl->line[(DWORD)lIdx % SYSLOG_LINES];
	for (i = 0; i < SYSLOG_LINELEN - 1 && psz[i]; i++)
		pszDst[i] = psz[i];
	pszDst[i] = 0;
}

void SysLog(const WCHAR *pszFmt, ...)
{
	WCHAR awchBuf[SYSLOG_LINELEN];
	va_list ap;
	va_start(ap, pszFmt);
	wvsprintfW(awchBuf, pszFmt, ap);
	va_end(ap);
	SysLogW(awchBuf);
}
