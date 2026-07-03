//
// ieproto.cpp - a winsock-backed Asynchronous Pluggable Protocol handler for "http:".
//
// CE Trident's WinInet doesn't fetch on the Dreamcast, and every way of injecting HTML into an
// already-loaded document fails (IPersistStreamInit::Load -> E_UNEXPECTED, IHTMLDocument2::write
// -> fault) and the filesystem is read-only (no file: trick). What DOES work is mshtml's normal
// bind/load pipeline (it renders about:blank and any navigation). So we register an
// IInternetProtocol that OVERRIDES the "http" namespace for this process: the control navigates
// http:// as usual, URLMON routes the bind to us, and we serve bytes fetched over winsock (the
// stack dcwnet proved works). Page, images, CSS and links all flow through here.
//
#include <windows.h>
#include <winsock.h>
#include <urlmon.h> // IInternetProtocol / IInternetSession (C++ interfaces - no CINTERFACE)

// {DC5A0001-7700-4d00-9000-000000000001} - our private handler CLSID
static const CLSID CLSID_DcwHttp = {
    0xdc5a0001, 0x7700, 0x4d00, {0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

// ---- winsock HTTP (self-contained; mirrors dcwnet) -------------------------------
#define HTTP_MAXRESP (512 * 1024)

static int AppendA(char *pszDst, const char *pszSrc)
{
	int i = 0;
	while (pszSrc[i])
	{
		pszDst[i] = pszSrc[i];
		i++;
	}
	return i;
}
static const char *StrStrA2(const char *pszHay, const char *pszNdl)
{
	int i, j;
	for (i = 0; pszHay[i]; i++)
	{
		for (j = 0; pszNdl[j] && pszHay[i + j] == pszNdl[j]; j++)
			;
		if (!pszNdl[j])
			return pszHay + i;
	}
	return 0;
}

static char *HttpFetch(const char *pszHost, int nPort, const char *pszPath, int *pcbOut)
{
	SOCKET s = INVALID_SOCKET;
	struct hostent *phe;
	SOCKADDR_IN sa;
	char *pBuf = NULL;
	int cb = 0, n, cbReq;
	char szReq[700];
	__try
	{
		phe = gethostbyname(pszHost);
		if (!phe || !phe->h_addr_list[0])
			return NULL;
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET)
			return NULL;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((u_short)nPort);
		memcpy(&sa.sin_addr, phe->h_addr_list[0], 4);
		if (connect(s, (SOCKADDR *)&sa, sizeof(sa)) != 0)
		{
			closesocket(s);
			return NULL;
		}
		cbReq = AppendA(szReq, "GET ");
		cbReq += AppendA(szReq + cbReq, pszPath);
		cbReq += AppendA(szReq + cbReq, " HTTP/1.0\r\nHost: ");
		cbReq += AppendA(szReq + cbReq, pszHost);
		cbReq +=
		    AppendA(szReq + cbReq, "\r\nUser-Agent: Mozilla/4.0 (compatible; DCWin)\r\nAccept: "
		                           "*/*\r\nConnection: close\r\n\r\n");
		send(s, szReq, cbReq, 0);
		pBuf = (char *)LocalAlloc(LMEM_FIXED, HTTP_MAXRESP);
		if (!pBuf)
		{
			closesocket(s);
			return NULL;
		}
		for (;;)
		{
			if (cb >= HTTP_MAXRESP - 4096)
				break;
			n = recv(s, pBuf + cb, 4096, 0);
			if (n <= 0)
				break;
			cb += n;
		}
		closesocket(s);
		pBuf[cb] = 0;
		*pcbOut = cb;
		return pBuf;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (s != INVALID_SOCKET)
			closesocket(s);
		if (pBuf)
			LocalFree(pBuf);
		return NULL;
	}
}

// Parse an http URL, GET it, follow one-scheme (http) redirects. Returns the LocalAlloc'd
// response (caller LocalFree) with the body offset/length filled in, or NULL.
static char *FetchUrlA(const char *pszUrlIn, int *pBodyOff, int *pBodyLen)
{
	char szUrl[600];
	int iHop, i = 0;
	while (pszUrlIn[i] && i < 599)
	{
		szUrl[i] = pszUrlIn[i];
		i++;
	}
	szUrl[i] = 0;

	for (iHop = 0; iHop < 4; iHop++)
	{
		const char *pszU = szUrl, *pszH;
		char szHost[256], szPath[512];
		int nPort = 80, k;
		if (pszU[0] == 'h' && pszU[1] == 't' && pszU[2] == 't' && pszU[3] == 'p' && pszU[4] == 's')
			return NULL; // no TLS
		if (pszU[0] == 'd' && pszU[1] == 'c' && pszU[2] == 'w' && pszU[3] == ':' &&
		    pszU[4] == '/' && pszU[5] == '/')
			pszU += 6; // our scheme
		else if (pszU[0] == 'h' && pszU[1] == 't' && pszU[2] == 't' && pszU[3] == 'p' &&
		         pszU[4] == ':' && pszU[5] == '/' && pszU[6] == '/')
			pszU += 7;
		for (k = 0; pszU[k] && pszU[k] != '/' && pszU[k] != ':' && k < 255; k++)
			szHost[k] = pszU[k];
		szHost[k] = 0;
		pszH = pszU + k;
		if (*pszH == ':')
		{
			nPort = 0;
			pszH++;
			while (*pszH >= '0' && *pszH <= '9')
				nPort = nPort * 10 + (*pszH++ - '0');
		}
		if (*pszH != '/')
		{
			szPath[0] = '/';
			szPath[1] = 0;
		}
		else
		{
			k = 0;
			while (pszH[k] && k < 510)
			{
				szPath[k] = pszH[k];
				k++;
			}
			szPath[k] = 0;
		}

		{
			int cb;
			char *pResp = HttpFetch(szHost, nPort, szPath, &cb);
			if (!pResp)
				return NULL;
			if (pResp[9] == '3') // redirect
			{
				const char *pszLoc = StrStrA2(pResp, "\nLocation:");
				if (!pszLoc)
					pszLoc = StrStrA2(pResp, "\nlocation:");
				if (pszLoc)
				{
					int j = 0;
					pszLoc += 10;
					while (*pszLoc == ' ')
						pszLoc++;
					while (*pszLoc && *pszLoc != '\r' && *pszLoc != '\n' && j < 599)
						szUrl[j++] = *pszLoc++;
					szUrl[j] = 0;
					LocalFree(pResp);
					continue;
				}
			}
			{
				const char *pszBody = StrStrA2(pResp, "\r\n\r\n");
				*pBodyOff = pszBody ? (int)(pszBody + 4 - pResp) : 0;
				*pBodyLen = cb - *pBodyOff;
			}
			return pResp;
		}
	}
	return NULL;
}

// ---- the per-request protocol object ---------------------------------------------
class CDcwHttp : public IInternetProtocol
{
	ULONG m_cRefs;
	char *m_pResp;
	int m_nBodyOff, m_nBodyLen, m_nPos;
	IInternetProtocolSink *m_pSink;
	int m_bResultDone;

  public:
	CDcwHttp()
	{
		m_cRefs = 1;
		m_pResp = NULL;
		m_nBodyOff = m_nBodyLen = m_nPos = 0;
		m_pSink = NULL;
		m_bResultDone = 0;
	}
	~CDcwHttp()
	{
		if (m_pResp)
			LocalFree(m_pResp);
		if (m_pSink)
			m_pSink->Release();
	}

	STDMETHOD(QueryInterface)(REFIID riid, void **ppv)
	{
		if (riid == IID_IUnknown || riid == IID_IInternetProtocolRoot ||
		    riid == IID_IInternetProtocol)
		{
			*ppv = (IInternetProtocol *)this;
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHOD_(ULONG, AddRef)(void)
	{
		return ++m_cRefs;
	}
	STDMETHOD_(ULONG, Release)(void)
	{
		if (--m_cRefs == 0)
		{
			delete this;
			return 0;
		}
		return m_cRefs;
	}

	// IInternetProtocolRoot
	STDMETHOD(Start)(LPCWSTR szUrl, IInternetProtocolSink *pSink, IInternetBindInfo *pBI,
	                 DWORD grfPI, DWORD dwReserved)
	{
		char szUrlA[600];
		(void)pBI;
		(void)grfPI;
		(void)dwReserved;
		WideCharToMultiByte(CP_ACP, 0, szUrl, -1, szUrlA, sizeof(szUrlA), NULL, NULL);
		OutputDebugStringW(L"proto: Start ");
		OutputDebugStringW(szUrl);
		OutputDebugStringW(L"\r\n");

		m_pResp = FetchUrlA(szUrlA, &m_nBodyOff, &m_nBodyLen);
		m_nPos = 0;
		if (!m_pResp)
		{
			OutputDebugStringW(L"proto: fetch FAILED\r\n");
			pSink->ReportResult(INET_E_DOWNLOAD_FAILURE, 0, NULL);
			return INET_E_DOWNLOAD_FAILURE;
		}
		{
			WCHAR b[64];
			wsprintfW(b, L"proto: %d bytes\r\n", m_nBodyLen);
			OutputDebugStringW(b);
		}
		m_pSink = pSink;
		m_pSink->AddRef(); // ReportResult is deferred until Read drains
		pSink->ReportProgress(BINDSTATUS_MIMETYPEAVAILABLE, L"text/html");
		// FULLYAVAILABLE: all data is ready; mshtml will Read until S_FALSE. We must NOT call
		// ReportResult yet - doing it before Read finishes makes mshtml re-bind + hang.
		pSink->ReportData(BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION |
		                      BSCF_DATAFULLYAVAILABLE,
		                  m_nBodyLen, m_nBodyLen);
		return S_OK;
	}
	STDMETHOD(Continue)(PROTOCOLDATA *)
	{
		return S_OK;
	}
	STDMETHOD(Abort)(HRESULT, DWORD)
	{
		return S_OK;
	}
	STDMETHOD(Terminate)(DWORD)
	{
		if (m_pSink)
		{
			m_pSink->Release();
			m_pSink = NULL;
		}
		return S_OK;
	}
	STDMETHOD(Suspend)(void)
	{
		return E_NOTIMPL;
	}
	STDMETHOD(Resume)(void)
	{
		return E_NOTIMPL;
	}

	// IInternetProtocol
	STDMETHOD(Read)(void *pv, ULONG cb, ULONG *pcbRead)
	{
		ULONG cbAvail, cbRead;
		if (!m_pResp)
		{
			if (pcbRead)
				*pcbRead = 0;
			return S_FALSE;
		}
		cbAvail = (ULONG)(m_nBodyLen - m_nPos);
		cbRead = (cb < cbAvail) ? cb : cbAvail;
		if (cbRead)
			memcpy(pv, m_pResp + m_nBodyOff + m_nPos, cbRead);
		m_nPos += (int)cbRead;
		if (pcbRead)
			*pcbRead = cbRead;
		if (m_nPos >= m_nBodyLen) // drained -> NOW signal the bind completed
		{
			if (m_pSink && !m_bResultDone)
			{
				m_bResultDone = 1;
				m_pSink->ReportResult(S_OK, 0, NULL);
			}
			return S_FALSE;
		}
		return S_OK;
	}
	STDMETHOD(Seek)(LARGE_INTEGER, DWORD, ULARGE_INTEGER *)
	{
		return E_NOTIMPL;
	}
	STDMETHOD(LockRequest)(DWORD)
	{
		return S_OK;
	}
	STDMETHOD(UnlockRequest)(void)
	{
		return S_OK;
	}
};

// ---- the class factory (a process-lifetime singleton) ----------------------------
class CDcwFactory : public IClassFactory
{
  public:
	STDMETHOD(QueryInterface)(REFIID riid, void **ppv)
	{
		if (riid == IID_IUnknown || riid == IID_IClassFactory)
		{
			*ppv = (IClassFactory *)this;
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHOD_(ULONG, AddRef)(void)
	{
		return 2;
	} // never freed
	STDMETHOD_(ULONG, Release)(void)
	{
		return 1;
	}
	STDMETHOD(CreateInstance)(IUnknown *pUnkOuter, REFIID riid, void **ppv)
	{
		CDcwHttp *pHttp;
		if (pUnkOuter)
			return CLASS_E_NOAGGREGATION;
		pHttp = new CDcwHttp();
		if (!pHttp)
			return E_OUTOFMEMORY;
		{
			HRESULT hr = pHttp->QueryInterface(riid, ppv);
			pHttp->Release();
			return hr;
		}
	}
	STDMETHOD(LockServer)(BOOL)
	{
		return S_OK;
	}
};

static CDcwFactory g_dcwFactory;

// Register our PRIVATE "dcw" scheme. We fully own it (no built-in fallback), so every bind
// phase mshtml does for a dcw:// URL is routed to us - unlike overriding "http", where the
// document-load phase fell back to (dead) WinInet and hung. iehost rewrites http://->dcw://.
extern "C" void IeRegisterWinsockProtocol(void)
{
	IInternetSession *pSess = NULL;
	if (CoInternetGetSession(0, &pSess, 0) == S_OK && pSess)
	{
		// dcw:// = the main document scheme (fully ours, no WinInet fallback). ALSO claim http:// and
		// https:// so the page's SUB-RESOURCE binds - images/CSS and especially the favicon Trident
		// auto-fetches after every page - reach us too, instead of falling into the image's dead
		// WinInet and hanging forever (the document then never reaches DocumentComplete, so the chrome
		// stays stuck on "opening page..."). http:// we fetch over winsock; https:// FetchUrlA refuses
		// (no TLS) -> a fast INET_E failure, so the resource is skipped and the page still completes.
		// (Unlike the earlier failed experiment, the MAIN document is still navigated as dcw://, so
		// this only intercepts resource binds - it doesn't route the document load through http.)
		HRESULT hrD = pSess->RegisterNameSpace(&g_dcwFactory, CLSID_DcwHttp, L"dcw", 0, NULL, 0);
		HRESULT hrH = pSess->RegisterNameSpace(&g_dcwFactory, CLSID_DcwHttp, L"http", 0, NULL, 0);
		HRESULT hrS = pSess->RegisterNameSpace(&g_dcwFactory, CLSID_DcwHttp, L"https", 0, NULL, 0);
		pSess->Release();
		{
			WCHAR b[96];
			wsprintfW(b, L"IE: register handlers dcw=%08x http=%08x https=%08x\r\n", (unsigned)hrD,
			          (unsigned)hrH, (unsigned)hrS);
			OutputDebugStringW(b);
		}
	}
	else
		OutputDebugStringW(L"IE: CoInternetGetSession FAILED\r\n");
}
