//
// sdcard.c - DCWin "External Storage": a WDM-loaded FAT file system driver that surfaces
// the SD card as "\External Storage" in the CE namespace (browsable in Explorer).
//
// Mechanism reversed from the GD-ROM driver wsegacd.dll (see memory wince-fsd-afs-mount):
// three CreateAPISet vtables (volume / file / find) registered with RegisterAPISet, then
// RegisterAFSName + RegisterAFS to mount. The volume callbacks delegate to ChaN FatFs;
// CreateFileW/FindFirstFileW mint per-object handles with CreateAPIHandle. A single
// critical section serialises all FatFs access (static LFN buffer is not re-entrant).
//
#include <windows.h>
#include <wdm.h>
#include "fatfs/ff.h"
#include "sdblk.h"
#include "syslog.h"

// ---- CE coredll API-set / AFS primitives (exported by name, no public header) ----------
typedef int (*APIFN)(void);
extern HANDLE WINAPI CreateAPISet(char *pName, DWORD dwMethods, const APIFN *ppfn,
                                  const DWORD *pdwSig);
extern HANDLE WINAPI CreateAPIHandle(HANDLE hAPISet, void *pvData);
extern BOOL WINAPI RegisterAPISet(HANDLE hAPISet, DWORD dwAPISet);
extern int WINAPI RegisterAFSName(const WCHAR *pName);
extern BOOL WINAPI RegisterAFS(int idx, HANDLE hAPISet, DWORD dwData, DWORD dwFlags);
extern BOOL WINAPI InitWDMDriver(HANDLE hInst, PDRIVER_INITIALIZE pfnEntry);

#define APISET_FILE 0x80000007 // handle-based file API class (from the reverse)
#define APISET_FIND 0x80000008 // handle-based find API class

// ---- state -----------------------------------------------------------------------------
static FATFS g_fs;
static int g_nAfsIdx = -1;
static HANDLE g_hVol, g_hFile, g_hFind;
static CRITICAL_SECTION g_cs;
static HINSTANCE g_hInst;

typedef struct
{
	FIL fil;
} SDFILE;
typedef struct
{
	DIR dir;
	WCHAR pat[64];
} SDFIND;

#define LOCK()   EnterCriticalSection(&g_cs)
#define UNLOCK() LeaveCriticalSection(&g_cs)

// ---- helpers ---------------------------------------------------------------------------
static int IsRoot(const WCHAR *psz)
{
	return psz == 0 || psz[0] == 0 || (psz[0] == L'\\' && psz[1] == 0) ||
	       (psz[0] == L'/' && psz[1] == 0);
}

// case-insensitive wildcard match ('*' and '?'); used to filter f_readdir output.
static int MatchPat(const WCHAR *pszPat, const WCHAR *pszName)
{
	while (*pszPat)
	{
		if (*pszPat == L'*')
		{
			pszPat++;
			if (!*pszPat)
				return 1;
			while (*pszName)
			{
				if (MatchPat(pszPat, pszName))
					return 1;
				pszName++;
			}
			return MatchPat(pszPat, pszName);
		}
		if (!*pszName)
			return 0;
		if (*pszPat != L'?')
		{
			WCHAR a = *pszPat, b = *pszName;
			if (a >= L'a' && a <= L'z')
				a -= 32;
			if (b >= L'a' && b <= L'z')
				b -= 32;
			if (a != b)
				return 0;
		}
		pszPat++;
		pszName++;
	}
	return *pszName == 0;
}

static void DosToFt(WORD wFdate, WORD wFtime, FILETIME *pFt)
{
	SYSTEMTIME st;
	memset(&st, 0, sizeof(st));
	st.wYear = (WORD)(1980 + (wFdate >> 9));
	st.wMonth = (WORD)((wFdate >> 5) & 0x0F);
	if (st.wMonth < 1)
		st.wMonth = 1;
	st.wDay = (WORD)(wFdate & 0x1F);
	if (st.wDay < 1)
		st.wDay = 1;
	st.wHour = (WORD)(wFtime >> 11);
	st.wMinute = (WORD)((wFtime >> 5) & 0x3F);
	st.wSecond = (WORD)((wFtime & 0x1F) * 2);
	SystemTimeToFileTime(&st, pFt);
}

// Fill a WIN32_FIND_DATAW from a FatFs FILINFO (+ its long name when present).
static void FillFind(WIN32_FIND_DATAW *pFd, FILINFO *pFno, const WCHAR *pszLfn)
{
	const WCHAR *pszNm = (pszLfn && pszLfn[0]) ? pszLfn : pFno->fname;
	int i;
	memset(pFd, 0, sizeof(*pFd));
	pFd->dwFileAttributes = pFno->fattrib; // FatFs AM_* == Win32 FILE_ATTRIBUTE_*
	pFd->nFileSizeLow = pFno->fsize;
	DosToFt(pFno->fdate, pFno->ftime, &pFd->ftLastWriteTime);
	pFd->ftCreationTime = pFd->ftLastWriteTime;
	pFd->ftLastAccessTime = pFd->ftLastWriteTime;
	for (i = 0; i < MAX_PATH - 1 && pszNm[i]; i++)
		pFd->cFileName[i] = pszNm[i];
	pFd->cFileName[i] = 0;
}

// ---- VOLUME callbacks (17; order per the reversed PCDF table) --------------------------
static int V_NoSupport(void)
{
	SetLastError(ERROR_NOT_SUPPORTED);
	return 0;
}

static BOOL V_CreateDirectory(void *pvVol, const WCHAR *pszPath, void *pvSa)
{
	BOOL bRet;
	(void)pvVol;
	(void)pvSa;
	LOCK();
	bRet = (f_mkdir(pszPath) == FR_OK);
	UNLOCK();
	return bRet;
}

static BOOL V_RemoveDirectory(void *pvVol, const WCHAR *pszPath)
{
	BOOL bRet;
	(void)pvVol;
	LOCK();
	bRet = (f_unlink(pszPath) == FR_OK);
	UNLOCK();
	return bRet;
}

static DWORD V_GetFileAttributes(void *pvVol, const WCHAR *pszPath)
{
	FILINFO fno;
	DWORD dwAttr;
	(void)pvVol;
	SysLog(L"sd: GetAttr '%s'", pszPath ? pszPath : L"(null)");
	if (IsRoot(pszPath))
		return FILE_ATTRIBUTE_DIRECTORY;
	LOCK();
	dwAttr = (f_stat(pszPath, &fno) == FR_OK) ? fno.fattrib : 0xFFFFFFFF;
	UNLOCK();
	return dwAttr;
}

static BOOL V_SetFileAttributes(void *pvVol, const WCHAR *pszPath, DWORD dwAttr)
{
	BOOL bRet;
	(void)pvVol;
	LOCK();
	bRet = (f_chmod(pszPath, (BYTE)dwAttr, AM_RDO | AM_ARC | AM_SYS | AM_HID) == FR_OK);
	UNLOCK();
	return bRet;
}

static HANDLE V_CreateFile(void *pvVol, HANDLE hProc, const WCHAR *pszName, DWORD dwAccess,
                           DWORD dwShare, void *pvSa, DWORD dwCreation, DWORD dwFlags, HANDLE hTmpl)
{
	SDFILE *pFile;
	BYTE bMode = 0;
	HANDLE h = INVALID_HANDLE_VALUE;
	(void)pvVol;
	(void)hProc;
	(void)dwShare;
	(void)pvSa;
	(void)dwFlags;
	(void)hTmpl;
	if (dwAccess & GENERIC_READ)
		bMode |= FA_READ;
	if (dwAccess & GENERIC_WRITE)
		bMode |= FA_WRITE;
	switch (dwCreation)
	{
		case CREATE_NEW:
			bMode |= FA_CREATE_NEW;
			break;
		case CREATE_ALWAYS:
			bMode |= FA_CREATE_ALWAYS;
			break;
		case OPEN_EXISTING:
			bMode |= FA_OPEN_EXISTING;
			break;
		case OPEN_ALWAYS:
			bMode |= FA_OPEN_ALWAYS;
			break;
		case TRUNCATE_EXISTING:
			bMode |= FA_CREATE_ALWAYS;
			break;
		default:
			bMode |= FA_OPEN_EXISTING;
			break;
	}
	pFile = (SDFILE *)LocalAlloc(LPTR, sizeof(SDFILE));
	if (!pFile)
	{
		SetLastError(ERROR_OUTOFMEMORY);
		return INVALID_HANDLE_VALUE;
	}
	LOCK();
	{
		FRESULT fr = f_open(&pFile->fil, pszName, bMode);
		SysLog(L"sd: CreateFile '%s' acc=%x cr=%d -> f_open=%d", pszName ? pszName : L"(null)",
		       dwAccess, dwCreation, fr);
		if (fr == FR_OK)
			h = CreateAPIHandle(g_hFile, pFile);
	}
	UNLOCK();
	if (h == 0 || h == INVALID_HANDLE_VALUE)
	{
		LocalFree(pFile);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

static BOOL V_DeleteFile(void *pvVol, const WCHAR *pszPath)
{
	BOOL bRet;
	(void)pvVol;
	LOCK();
	bRet = (f_unlink(pszPath) == FR_OK);
	UNLOCK();
	return bRet;
}

static BOOL V_MoveFile(void *pvVol, const WCHAR *pszOldp, const WCHAR *pszNewp)
{
	BOOL bRet;
	(void)pvVol;
	LOCK();
	bRet = (f_rename(pszOldp, pszNewp) == FR_OK);
	UNLOCK();
	return bRet;
}

static HANDLE V_FindFirstFile(void *pvVol, HANDLE hProc, const WCHAR *pszSpec,
                              WIN32_FIND_DATAW *pFd)
{
	SDFIND *pFind;
	WCHAR szDir[MAX_PATH], szLfn[256];
	FILINFO fno;
	const WCHAR *pszPat;
	int i, nSlash = -1;
	HANDLE h = INVALID_HANDLE_VALUE;
	(void)pvVol;
	(void)hProc;
	for (i = 0; pszSpec[i] && i < MAX_PATH - 1; i++)
		if (pszSpec[i] == L'\\' || pszSpec[i] == L'/')
			nSlash = i;
	for (i = 0; i < nSlash; i++)
		szDir[i] = pszSpec[i];
	szDir[nSlash > 0 ? nSlash : 0] = 0; // dir = path up to last slash ("" = root)
	pszPat = pszSpec + nSlash + 1;      // pattern after last slash

	pFind = (SDFIND *)LocalAlloc(LPTR, sizeof(SDFIND));
	if (!pFind)
	{
		SetLastError(ERROR_OUTOFMEMORY);
		return INVALID_HANDLE_VALUE;
	}
	for (i = 0; i < 63 && pszPat[i]; i++)
		pFind->pat[i] = pszPat[i];
	pFind->pat[i] = 0;
	if (!pFind->pat[0])
	{
		pFind->pat[0] = L'*';
		pFind->pat[1] = 0;
	} // empty spec -> match all

	SysLog(L"sd: FindFirst '%s' pat='%s'", pszSpec ? pszSpec : L"(null)", pFind->pat);
	LOCK();
	{
		FRESULT fr = f_opendir(&pFind->dir, IsRoot(szDir) ? L"\\" : szDir);
		SysLog(L"sd: f_opendir('%s')=%d", IsRoot(szDir) ? L"\\" : szDir, fr);
		if (fr == FR_OK)
			for (;;)
			{
				fno.lfname = szLfn;
				fno.lfsize = 256;
				szLfn[0] = 0;
				if (f_readdir(&pFind->dir, &fno) != FR_OK || fno.fname[0] == 0)
					break; // end
				if (MatchPat(pFind->pat, szLfn[0] ? szLfn : fno.fname))
				{
					FillFind(pFd, &fno, szLfn);
					h = CreateAPIHandle(g_hFind, pFind);
					break;
				}
			}
	}
	UNLOCK();
	if (h == 0 || h == INVALID_HANDLE_VALUE)
	{
		f_closedir(&pFind->dir);
		LocalFree(pFind);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

static BOOL V_GetDiskFreeSpace(void *pvVol, const WCHAR *pszPath, DWORD *pdwSpc, DWORD *pdwBps,
                               DWORD *pdwFreecl, DWORD *pdwTotcl)
{
	FATFS *pFs;
	DWORD dwNfree;
	BOOL bRet;
	(void)pvVol;
	(void)pszPath;
	LOCK();
	bRet = (f_getfree(L"", &dwNfree, &pFs) == FR_OK);
	UNLOCK();
	if (!bRet)
		return FALSE;
	if (pdwSpc)
		*pdwSpc = pFs->csize;
	if (pdwBps)
		*pdwBps = 512;
	if (pdwFreecl)
		*pdwFreecl = dwNfree;
	if (pdwTotcl)
		*pdwTotcl = pFs->n_fatent - 2;
	return TRUE;
}

// ---- FILE callbacks (14; order per the reversed HCDF table) ----------------------------
// GetFileInformationByHandle (slot 6): the CE module loader calls this to size/validate an
// image before paging its code sections. wsegacd (which launches \CD-ROM\DC.EXE) fills a full
// BY_HANDLE_FILE_INFORMATION and returns TRUE; our old V_NoSupport stub returned FALSE -> the
// loader faulted -> kernel reset. Fill attributes + size + (zero) times + dwOID like wsegacd.
static BOOL F_GetInfo(SDFILE *pFile, BY_HANDLE_FILE_INFORMATION *pBhfi)
{
	DWORD dwSz;
	if (!pFile || !pBhfi)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	LOCK();
	dwSz = f_size(&pFile->fil);
	UNLOCK();
	memset(pBhfi, 0, sizeof(*pBhfi));
	pBhfi->dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
	pBhfi->nFileSizeLow = dwSz;
	pBhfi->nNumberOfLinks = 1;
	pBhfi->dwOID = 0xFFFFFFFF;
	SysLog(L"sd: F_GetInfo -> size=%u", dwSz);
	return TRUE;
}

// GetFileTime (slot 8): loader may probe it; return zeroed (valid) times, TRUE.
static BOOL F_GetFileTime(SDFILE *pFile, FILETIME *pCre, FILETIME *pAcc, FILETIME *pWri)
{
	FILETIME z = {0, 0};
	if (!pFile)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (pCre)
		*pCre = z;
	if (pAcc)
		*pAcc = z;
	if (pWri)
		*pWri = z;
	return TRUE;
}

static int F_UnsupSetTime(void)
{
	SysLog(L"sd: file SetFileTime (slot9) called");
	SetLastError(ERROR_NOT_SUPPORTED);
	return 0;
}
static int F_UnsupIoctl(void)
{
	SysLog(L"sd: file DeviceIoControl (slot11) called");
	SetLastError(ERROR_NOT_SUPPORTED);
	return 0;
}

static BOOL F_Close(SDFILE *pFile)
{
	BOOL bRet;
	if (!pFile)
		return FALSE;
	SysLog(L"sd: F_Close");
	LOCK();
	bRet = (f_close(&pFile->fil) == FR_OK);
	UNLOCK();
	LocalFree(pFile);
	return bRet;
}

static BOOL F_Read(SDFILE *pFile, void *pvBuf, DWORD cb, DWORD *pcb, void *pvOvl)
{
	UINT br = 0;
	BOOL bRet;
	(void)pvOvl;
	if (!pFile)
		return FALSE;
	LOCK();
	bRet = (f_read(&pFile->fil, pvBuf, cb, &br) == FR_OK);
	UNLOCK();
	if (pcb)
		*pcb = br;
	SysLog(L"sd: F_Read cb=%u -> r=%d br=%u", cb, bRet, br);
	return bRet;
}

static BOOL F_Write(SDFILE *pFile, const void *pvBuf, DWORD cb, DWORD *pcb, void *pvOvl)
{
	UINT bw = 0;
	BOOL bRet;
	(void)pvOvl;
	if (!pFile)
		return FALSE;
	LOCK();
	bRet = (f_write(&pFile->fil, pvBuf, cb, &bw) == FR_OK);
	UNLOCK();
	if (pcb)
		*pcb = bw;
	return bRet;
}

static DWORD F_GetSize(SDFILE *pFile, DWORD *pdwHigh)
{
	DWORD dwSz;
	if (!pFile)
		return 0;
	LOCK();
	dwSz = f_size(&pFile->fil);
	UNLOCK();
	if (pdwHigh)
		*pdwHigh = 0;
	SysLog(L"sd: F_GetSize -> %u", dwSz);
	return dwSz;
}

static DWORD F_SetPointer(SDFILE *pFile, LONG lLo, LONG *plHigh, DWORD dwMethod)
{
	DWORD dwPos;
	(void)plHigh;
	if (!pFile)
		return 0xFFFFFFFF;
	LOCK();
	if (dwMethod == FILE_CURRENT)
		lLo += (LONG)f_tell(&pFile->fil);
	else if (dwMethod == FILE_END)
		lLo += (LONG)f_size(&pFile->fil);
	f_lseek(&pFile->fil, (DWORD)lLo);
	dwPos = f_tell(&pFile->fil);
	UNLOCK();
	return dwPos;
}

static BOOL F_Flush(SDFILE *pFile)
{
	BOOL bRet;
	if (!pFile)
		return FALSE;
	LOCK();
	bRet = (f_sync(&pFile->fil) == FR_OK);
	UNLOCK();
	return bRet;
}

static BOOL F_SetEnd(SDFILE *pFile)
{
	BOOL bRet;
	if (!pFile)
		return FALSE;
	LOCK();
	bRet = (f_truncate(&pFile->fil) == FR_OK);
	UNLOCK();
	return bRet;
}

static BOOL F_ReadSeek(SDFILE *pFile, void *pvBuf, DWORD cb, DWORD *pcb, void *pvOvl, DWORD dwLo,
                       DWORD dwHi)
{
	UINT br = 0;
	BOOL bRet;
	(void)pvOvl;
	(void)dwHi;
	if (!pFile)
		return FALSE;
	LOCK();
	f_lseek(&pFile->fil, dwLo);
	bRet = (f_read(&pFile->fil, pvBuf, cb, &br) == FR_OK);
	UNLOCK();
	if (pcb)
		*pcb = br;
	SysLog(L"sd: F_ReadSeek off=%u cb=%u -> r=%d br=%u", dwLo, cb, bRet, br);
	return bRet;
}

static BOOL F_WriteSeek(SDFILE *pFile, const void *pvBuf, DWORD cb, DWORD *pcb, void *pvOvl,
                        DWORD dwLo, DWORD dwHi)
{
	UINT bw = 0;
	BOOL bRet;
	(void)pvOvl;
	(void)dwHi;
	if (!pFile)
		return FALSE;
	LOCK();
	f_lseek(&pFile->fil, dwLo);
	bRet = (f_write(&pFile->fil, pvBuf, cb, &bw) == FR_OK);
	UNLOCK();
	if (pcb)
		*pcb = bw;
	return bRet;
}

// ---- FIND callbacks (3; order per the reversed FCDF table) -----------------------------
static BOOL S_Close(SDFIND *pFind)
{
	if (!pFind)
		return FALSE;
	LOCK();
	f_closedir(&pFind->dir);
	UNLOCK();
	LocalFree(pFind);
	return TRUE;
}

static BOOL S_Next(SDFIND *pFind, WIN32_FIND_DATAW *pFd)
{
	WCHAR szLfn[256];
	FILINFO fno;
	BOOL bRet = FALSE;
	if (!pFind)
		return FALSE;
	LOCK();
	for (;;)
	{
		fno.lfname = szLfn;
		fno.lfsize = 256;
		szLfn[0] = 0;
		if (f_readdir(&pFind->dir, &fno) != FR_OK || fno.fname[0] == 0)
			break;
		if (MatchPat(pFind->pat, szLfn[0] ? szLfn : fno.fname))
		{
			FillFind(pFd, &fno, szLfn);
			bRet = TRUE;
			break;
		}
	}
	UNLOCK();
	if (!bRet)
		SetLastError(ERROR_NO_MORE_FILES);
	return bRet;
}

// ---- API-set method + signature tables (exact slot order + sigs from the reverse) ------
static const APIFN g_volMethods[17] = {(APIFN)V_NoSupport,
                                       (APIFN)V_NoSupport,
                                       (APIFN)V_CreateDirectory,
                                       (APIFN)V_RemoveDirectory,
                                       (APIFN)V_GetFileAttributes,
                                       (APIFN)V_SetFileAttributes,
                                       (APIFN)V_CreateFile,
                                       (APIFN)V_DeleteFile,
                                       (APIFN)V_MoveFile,
                                       (APIFN)V_FindFirstFile,
                                       (APIFN)V_NoSupport /*RegFSNotify*/,
                                       (APIFN)V_NoSupport /*OidGetInfo*/,
                                       (APIFN)V_MoveFile /*DeleteAndRename*/,
                                       (APIFN)V_NoSupport /*CloseAllHandles*/,
                                       (APIFN)V_GetDiskFreeSpace,
                                       (APIFN)V_NoSupport /*Notify*/,
                                       (APIFN)V_NoSupport /*RegFSFunc*/};
static const DWORD g_volSigs[17] = {0,    0, 0x14, 4,    4, 4,     0x410, 4, 0x14,
                                    0x50, 0, 0x10, 0x14, 0, 0x554, 0,     0};

static const APIFN g_fileMethods[14] = {(APIFN)F_Close,
                                        (APIFN)V_NoSupport,
                                        (APIFN)F_Read,
                                        (APIFN)F_Write,
                                        (APIFN)F_GetSize,
                                        (APIFN)F_SetPointer,
                                        (APIFN)F_GetInfo /*GetFileInfo*/,
                                        (APIFN)F_Flush,
                                        (APIFN)F_GetFileTime /*GetFileTime*/,
                                        (APIFN)F_UnsupSetTime /*SetFileTime*/,
                                        (APIFN)F_SetEnd,
                                        (APIFN)F_UnsupIoctl /*DeviceIoControl*/,
                                        (APIFN)F_ReadSeek,
                                        (APIFN)F_WriteSeek};
static const DWORD g_fileSigs[14] = {0, 0,    0x144, 0x144, 4,      0x10,  4,
                                     0, 0x54, 0x54,  0,     0x5110, 0x144, 0x144};

static const APIFN g_findMethods[3] = {(APIFN)S_Close, (APIFN)V_NoSupport, (APIFN)S_Next};
static const DWORD g_findSigs[3] = {0, 0, 4};

// ---- mount + WDM entry -----------------------------------------------------------------
static void FsdInit(void)
{
	InitializeCriticalSection(&g_cs);
	g_hVol = CreateAPISet("FATV", 17, g_volMethods, g_volSigs);
	g_hFile = CreateAPISet("FATH", 14, g_fileMethods, g_fileSigs);
	g_hFind = CreateAPISet("FATF", 3, g_findMethods, g_findSigs);
	if (!g_hVol || !g_hFile || !g_hFind)
		return;
	RegisterAPISet(g_hFile, APISET_FILE);
	RegisterAPISet(g_hFind, APISET_FIND);

	f_mount(&g_fs, L"", 0); // lazy: the disk is initialised on first access

	g_nAfsIdx = RegisterAFSName(L"External Storage");
	if (g_nAfsIdx >= 0)
	{
		BOOL bRet = RegisterAFS(g_nAfsIdx, g_hVol, 1, 4); // dwData unused (single volume)
		SysLog(L"sd: FsdInit afs=%d RegisterAFS=%d", g_nAfsIdx, bRet);
	}
	else
		SysLog(L"sd: RegisterAFSName failed");
}

static NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	(void)drv;
	(void)reg;
	FsdInit();
	return 0; // STATUS_SUCCESS
}

BOOL WINAPI DllMain(HANDLE hInst, DWORD dwReason, LPVOID pvReserved)
{
	(void)pvReserved;
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		g_hInst = (HINSTANCE)hInst;
		return InitWDMDriver(hInst, DriverEntry);
	}
	return TRUE;
}
