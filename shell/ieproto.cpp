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

static int AppendA(char *dst, const char *src)
{
	int i = 0;
	while (src[i])
	{
		dst[i] = src[i];
		i++;
	}
	return i;
}
static const char *StrStrA2(const char *hay, const char *ndl)
{
	int i, j;
	for (i = 0; hay[i]; i++)
	{
		for (j = 0; ndl[j] && hay[i + j] == ndl[j]; j++)
			;
		if (!ndl[j])
			return hay + i;
	}
	return 0;
}

static char *HttpFetch(const char *host, int port, const char *path, int *outLen)
{
	SOCKET s = INVALID_SOCKET;
	struct hostent *he;
	SOCKADDR_IN sa;
	char *buf = NULL;
	int len = 0, n, rl;
	char req[700];
	__try
	{
		he = gethostbyname(host);
		if (!he || !he->h_addr_list[0])
			return NULL;
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET)
			return NULL;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((u_short)port);
		memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
		if (connect(s, (SOCKADDR *)&sa, sizeof(sa)) != 0)
		{
			closesocket(s);
			return NULL;
		}
		rl = AppendA(req, "GET ");
		rl += AppendA(req + rl, path);
		rl += AppendA(req + rl, " HTTP/1.0\r\nHost: ");
		rl += AppendA(req + rl, host);
		rl += AppendA(req + rl, "\r\nUser-Agent: Mozilla/4.0 (compatible; DCWin)\r\nAccept: "
		                        "*/*\r\nConnection: close\r\n\r\n");
		send(s, req, rl, 0);
		buf = (char *)LocalAlloc(LMEM_FIXED, HTTP_MAXRESP);
		if (!buf)
		{
			closesocket(s);
			return NULL;
		}
		for (;;)
		{
			if (len >= HTTP_MAXRESP - 4096)
				break;
			n = recv(s, buf + len, 4096, 0);
			if (n <= 0)
				break;
			len += n;
		}
		closesocket(s);
		buf[len] = 0;
		*outLen = len;
		return buf;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (s != INVALID_SOCKET)
			closesocket(s);
		if (buf)
			LocalFree(buf);
		return NULL;
	}
}

// Parse an http URL, GET it, follow one-scheme (http) redirects. Returns the LocalAlloc'd
// response (caller LocalFree) with the body offset/length filled in, or NULL.
static char *FetchUrlA(const char *urlIn, int *bodyOff, int *bodyLen)
{
	char url[600];
	int hop, i = 0;
	while (urlIn[i] && i < 599)
	{
		url[i] = urlIn[i];
		i++;
	}
	url[i] = 0;

	for (hop = 0; hop < 4; hop++)
	{
		const char *u = url, *h;
		char host[256], path[512];
		int port = 80, k;
		if (u[0] == 'h' && u[1] == 't' && u[2] == 't' && u[3] == 'p' && u[4] == 's')
			return NULL; // no TLS
		if (u[0] == 'd' && u[1] == 'c' && u[2] == 'w' && u[3] == ':' && u[4] == '/' && u[5] == '/')
			u += 6; // our scheme
		else if (u[0] == 'h' && u[1] == 't' && u[2] == 't' && u[3] == 'p' && u[4] == ':' &&
		         u[5] == '/' && u[6] == '/')
			u += 7;
		for (k = 0; u[k] && u[k] != '/' && u[k] != ':' && k < 255; k++)
			host[k] = u[k];
		host[k] = 0;
		h = u + k;
		if (*h == ':')
		{
			port = 0;
			h++;
			while (*h >= '0' && *h <= '9')
				port = port * 10 + (*h++ - '0');
		}
		if (*h != '/')
		{
			path[0] = '/';
			path[1] = 0;
		}
		else
		{
			k = 0;
			while (h[k] && k < 510)
			{
				path[k] = h[k];
				k++;
			}
			path[k] = 0;
		}

		{
			int len;
			char *resp = HttpFetch(host, port, path, &len);
			if (!resp)
				return NULL;
			if (resp[9] == '3') // redirect
			{
				const char *loc = StrStrA2(resp, "\nLocation:");
				if (!loc)
					loc = StrStrA2(resp, "\nlocation:");
				if (loc)
				{
					int j = 0;
					loc += 10;
					while (*loc == ' ')
						loc++;
					while (*loc && *loc != '\r' && *loc != '\n' && j < 599)
						url[j++] = *loc++;
					url[j] = 0;
					LocalFree(resp);
					continue;
				}
			}
			{
				const char *body = StrStrA2(resp, "\r\n\r\n");
				*bodyOff = body ? (int)(body + 4 - resp) : 0;
				*bodyLen = len - *bodyOff;
			}
			return resp;
		}
	}
	return NULL;
}

// ---- the per-request protocol object ---------------------------------------------
class CDcwHttp : public IInternetProtocol
{
	ULONG _refs;
	char *_resp;
	int _bodyOff, _bodyLen, _pos;
	IInternetProtocolSink *_pSink;
	int _resultDone;

  public:
	CDcwHttp()
	{
		_refs = 1;
		_resp = NULL;
		_bodyOff = _bodyLen = _pos = 0;
		_pSink = NULL;
		_resultDone = 0;
	}
	~CDcwHttp()
	{
		if (_resp)
			LocalFree(_resp);
		if (_pSink)
			_pSink->Release();
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
		return ++_refs;
	}
	STDMETHOD_(ULONG, Release)(void)
	{
		if (--_refs == 0)
		{
			delete this;
			return 0;
		}
		return _refs;
	}

	// IInternetProtocolRoot
	STDMETHOD(Start)(LPCWSTR szUrl, IInternetProtocolSink *pSink, IInternetBindInfo *pBI,
	                 DWORD grfPI, DWORD dwReserved)
	{
		char urlA[600];
		(void)pBI;
		(void)grfPI;
		(void)dwReserved;
		WideCharToMultiByte(CP_ACP, 0, szUrl, -1, urlA, sizeof(urlA), NULL, NULL);
		OutputDebugStringW(L"proto: Start ");
		OutputDebugStringW(szUrl);
		OutputDebugStringW(L"\r\n");

		_resp = FetchUrlA(urlA, &_bodyOff, &_bodyLen);
		_pos = 0;
		if (!_resp)
		{
			OutputDebugStringW(L"proto: fetch FAILED\r\n");
			pSink->ReportResult(INET_E_DOWNLOAD_FAILURE, 0, NULL);
			return INET_E_DOWNLOAD_FAILURE;
		}
		{
			WCHAR b[64];
			wsprintfW(b, L"proto: %d bytes\r\n", _bodyLen);
			OutputDebugStringW(b);
		}
		_pSink = pSink;
		_pSink->AddRef(); // ReportResult is deferred until Read drains
		pSink->ReportProgress(BINDSTATUS_MIMETYPEAVAILABLE, L"text/html");
		// FULLYAVAILABLE: all data is ready; mshtml will Read until S_FALSE. We must NOT call
		// ReportResult yet - doing it before Read finishes makes mshtml re-bind + hang.
		pSink->ReportData(BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION |
		                      BSCF_DATAFULLYAVAILABLE,
		                  _bodyLen, _bodyLen);
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
		if (_pSink)
		{
			_pSink->Release();
			_pSink = NULL;
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
		ULONG avail, nn;
		if (!_resp)
		{
			if (pcbRead)
				*pcbRead = 0;
			return S_FALSE;
		}
		avail = (ULONG)(_bodyLen - _pos);
		nn = (cb < avail) ? cb : avail;
		if (nn)
			memcpy(pv, _resp + _bodyOff + _pos, nn);
		_pos += (int)nn;
		if (pcbRead)
			*pcbRead = nn;
		if (_pos >= _bodyLen) // drained -> NOW signal the bind completed
		{
			if (_pSink && !_resultDone)
			{
				_resultDone = 1;
				_pSink->ReportResult(S_OK, 0, NULL);
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
		CDcwHttp *p;
		if (pUnkOuter)
			return CLASS_E_NOAGGREGATION;
		p = new CDcwHttp();
		if (!p)
			return E_OUTOFMEMORY;
		{
			HRESULT hr = p->QueryInterface(riid, ppv);
			p->Release();
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
		HRESULT hr = pSess->RegisterNameSpace(&g_dcwFactory, CLSID_DcwHttp, L"dcw", 0, NULL, 0);
		pSess->Release();
		{
			WCHAR b[64];
			wsprintfW(b, L"IE: register http handler hr=%08x\r\n", (unsigned)hr);
			OutputDebugStringW(b);
		}
	}
	else
		OutputDebugStringW(L"IE: CoInternetGetSession FAILED\r\n");
}
