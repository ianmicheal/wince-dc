//
// dcwnet.c - Network diagnostics: a DCWin client that exercises the whole stack the way a
// real winsock title does - including DIALING. The old version only did gethostbyname/connect,
// which works on ethernet (the link is already up via boot-time DHCP) but NEVER brings a modem
// up: dial-up only connects when something calls RasDial. So this version dials first (RasDial
// -> our mppp shim -> ethernet instant-connect OR, in modem mode, the delegated original PPP
// driver), shows the connection + IP + DNS, then resolves + connects to a user-typed target.
//
// All controls are clickable with the analog-stick pointer (the shell delivers it via
// DCWinGetPointer): an on-screen keyboard edits the target host/IP, and Dial / Test / Hang Up
// buttons drive the test. Results are a colour-coded log (green OK / red FAIL / blue info).
//
#include "dcwlib.h"
#include <winsock.h>
#include <ras.h>

#define CW 472
#define CH 372

// ---- colours ----
#define C_BG    RGB(192, 192, 192)
#define C_HDR   RGB(0, 0, 128)
#define C_WHITE RGB(255, 255, 255)
#define C_BLACK RGB(0, 0, 0)
#define C_KEY   RGB(176, 176, 176)
#define C_KEYF  RGB(224, 224, 224)
#define C_BTN   RGB(0, 0, 128)
#define C_OK    RGB(0, 112, 0)
#define C_FAIL  RGB(176, 0, 0)
#define C_INFO  RGB(0, 0, 160)
#define C_MUTE  RGB(96, 96, 96)

// ---- target host/IP (optionally host/path) being edited ----
// Default to a known plain-HTTP test file (exactly 1,048,576 bytes, Content-Length set) so the
// Get download test works out of the box - we have no TLS, so it must be http:// (port 80).
static WCHAR g_target[80] = L"speedtest.tele2.net/1MB.zip";

// ---- connection state ----
enum
{
	CN_IDLE,
	CN_DIALING,
	CN_UP,
	CN_FAILED
};
static int g_conn = CN_IDLE;
static HRASCONN g_hConn;
static DWORD g_dialStart;
static WCHAR g_ipStr[24] = L"-";
static WCHAR g_dnsStr[24] = L"-";

// ---- colour-coded result log ----
typedef struct
{
	COLORREF c;
	WCHAR s[58];
} LogLine;
static LogLine g_log[7];
static int g_nLog;

static void LogC(COLORREF c, const WCHAR *s)
{
	int i;
	if (g_nLog >= 7)
	{
		for (i = 0; i < 6; i++)
			g_log[i] = g_log[i + 1];
		g_nLog = 6;
	} // scroll
	g_log[g_nLog].c = c;
	for (i = 0; i < 56 && s[i]; i++)
		g_log[g_nLog].s[i] = s[i];
	g_log[g_nLog].s[i] = 0;
	g_nLog++;
}
static void LogF(COLORREF c, const WCHAR *fmt, DWORD a)
{
	WCHAR b[58];
	wsprintfW(b, fmt, a);
	LogC(c, b);
}

// ---- registry DNS (the shim writes HKLM\Comm "DnsServers" = [count][ip...] net order) ----
static unsigned long ReadDns(void)
{
	HKEY h;
	DWORD t, n;
	unsigned long buf[6], ip = 0;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Comm", 0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS)
	{
		n = sizeof(buf);
		if (RegQueryValueExW(h, L"DnsServers", 0, &t, (BYTE *)buf, &n) == ERROR_SUCCESS && n >= 8 &&
		    buf[0] >= 1)
			ip = buf[1];
		RegCloseKey(h);
	}
	return ip;
}

static void IpToStr(unsigned long ip, WCHAR *out)
{
	unsigned char *p = (unsigned char *)&ip;
	wsprintfW(out, L"%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

// Local bound address: a UDP socket "connected" to the DNS server (no packets sent) lets
// getsockname report the source IP the stack would use - works on ethernet and PPP alike.
static void RefreshLocalIp(void)
{
	unsigned long dns = ReadDns();
	SOCKET s;
	SOCKADDR_IN sa, me;
	int ml = sizeof(me);
	if (dns)
		IpToStr(dns, g_dnsStr);
	else
		lstrcpyW(g_dnsStr, L"(none)");
	lstrcpyW(g_ipStr, L"-");
	if (!dns)
		return;
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == INVALID_SOCKET)
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(53);
	sa.sin_addr.s_addr = dns;
	if (connect(s, (SOCKADDR *)&sa, sizeof(sa)) == 0 && getsockname(s, (SOCKADDR *)&me, &ml) == 0)
		IpToStr(me.sin_addr.s_addr, g_ipStr);
	closesocket(s);
}

// ---- dialing (RasDial -> our shim -> modem PPP / ethernet instant-connect) ----
// Ensure a dial entry exists pointing at the DC built-in modem, so RasDial has something to
// dial. On ethernet our shim ignores the device and instant-connects; in modem mode the
// delegated original driver opens HKLM\Modem\Sega-DreamcastBuiltIn (COM6) and runs PPP.
static void EnsureEntry(void)
{
	RASENTRY re;
	memset(&re, 0, sizeof(re));
	re.dwSize = sizeof(re);
	re.dwfOptions = RASEO_IpHeaderCompression;
	re.dwCountryID = 1;
	lstrcpyW(re.szDeviceType, RASDT_Modem);
	lstrcpyW(re.szDeviceName, L"Sega-DreamcastBuiltIn");
	lstrcpyW(re.szLocalPhoneNumber, L"0118999"); // Flycast/null-modem ignore the number
	RasSetEntryProperties(NULL, L"DC Modem", &re, sizeof(re), NULL, 0);
}

static void DoDial(void)
{
	RASDIALPARAMS p;
	DWORD rc;
	if (g_conn == CN_DIALING || g_conn == CN_UP)
		return;
	g_nLog = 0;
	LogC(C_INFO, L"Dialing (RasDial)...");
	EnsureEntry();
	memset(&p, 0, sizeof(p));
	p.dwSize = sizeof(p);
	lstrcpyW(p.szEntryName, L"DC Modem");
	g_hConn = 0;
	rc = RasDial(NULL, NULL, &p, 0, NULL, &g_hConn);
	if (rc != 0)
	{
		LogF(C_FAIL, L"RasDial err %u", rc);
		g_conn = CN_FAILED;
		return;
	}
	g_conn = CN_DIALING;
	g_dialStart = GetTickCount();
}

// Non-blocking dial progress; called each frame while CN_DIALING. Returns 1 if state changed.
static int PollDial(void)
{
	RASCONNSTATUS st;
	if (g_conn != CN_DIALING)
		return 0;
	memset(&st, 0, sizeof(st));
	st.dwSize = sizeof(st);
	RasGetConnectStatus(g_hConn, &st);
	if (st.rasconnstate == RASCS_Connected)
	{
		WCHAR b[58];
		g_conn = CN_UP;
		RefreshLocalIp();
		LogC(C_OK, L"Connected.");
		wsprintfW(b, L"  IP  %s", g_ipStr);
		LogC(C_INFO, b);
		wsprintfW(b, L"  DNS %s", g_dnsStr);
		LogC(C_INFO, b);
		return 1;
	}
	if (st.dwError != 0)
	{
		LogF(C_FAIL, L"Dial failed, err %u", st.dwError);
		g_conn = CN_FAILED;
		return 1;
	}
	if (GetTickCount() - g_dialStart > 45000)
	{
		LogC(C_FAIL, L"Dial timeout (45s)");
		g_conn = CN_FAILED;
		return 1;
	}
	return 0;
}

static void DoHangup(void)
{
	if (g_hConn)
		RasHangUp(g_hConn);
	g_hConn = 0;
	g_conn = CN_IDLE;
	lstrcpyW(g_ipStr, L"-");
	LogC(C_MUTE, L"Hung up.");
}

// ---- the actual reachability test (DNS -> TCP -> HTTP), on g_target ----
static int TryConnect(SOCKET s, SOCKADDR_IN *sa, int secs)
{
	unsigned long nb = 1;
	struct timeval tv;
	fd_set wf, ef;
	int r;
	ioctlsocket(s, FIONBIO, &nb);
	if (connect(s, (SOCKADDR *)sa, sizeof(*sa)) == 0)
		return 1;
	if (WSAGetLastError() != WSAEWOULDBLOCK)
		return 0;
	FD_ZERO(&wf);
	FD_SET(s, &wf);
	FD_ZERO(&ef);
	FD_SET(s, &ef);
	tv.tv_sec = secs;
	tv.tv_usec = 0;
	r = select(0, 0, &wf, &ef, &tv);
	return (r > 0 && FD_ISSET(s, &wf)) ? 1 : 0;
}

// Split g_target ("host" or "host/path") into an ANSI host + path (path defaults to "/").
static void SplitTarget(char *hostA, int hcap, char *pathA, int pcap)
{
	WCHAR host[80];
	const WCHAR *slash;
	int i, hl = 0;
	for (slash = g_target; *slash && *slash != L'/'; slash++)
		;
	for (i = 0; g_target + i < slash && hl < 78; i++)
		host[hl++] = g_target[i];
	host[hl] = 0;
	WideCharToMultiByte(CP_ACP, 0, host, -1, hostA, hcap, 0, 0);
	if (*slash)
		WideCharToMultiByte(CP_ACP, 0, slash, -1, pathA, pcap, 0, 0);
	else
	{
		pathA[0] = '/';
		pathA[1] = 0;
	}
}

static void DoTest(void)
{
	char hostA[80], pathA[96];
	struct hostent *he;
	unsigned long ip;
	SOCKET s;
	SOCKADDR_IN sa;
	int isIp;

	g_nLog = 0;
	if (g_conn != CN_UP)
		LogC(C_MUTE, L"(not dialed - testing anyway)");
	SplitTarget(hostA, sizeof(hostA), pathA, sizeof(pathA));

	// numeric IP or hostname?
	ip = inet_addr(hostA);
	isIp = (ip != INADDR_NONE);
	if (isIp)
	{
		WCHAR b[40];
		wsprintfW(b, L"target is literal IP");
		LogC(C_INFO, b);
	}
	else
	{
		__try
		{
			he = gethostbyname(hostA);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			he = 0;
		}
		if (he && he->h_addr_list[0])
		{
			WCHAR b[58], ips[24];
			ip = *(unsigned long *)he->h_addr_list[0];
			IpToStr(ip, ips);
			wsprintfW(b, L"DNS OK -> %s", ips);
			LogC(C_OK, b);
		}
		else
		{
			LogC(C_FAIL, L"DNS resolve FAILED");
			return;
		}
	}

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
	{
		LogC(C_FAIL, L"socket() failed");
		return;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	sa.sin_addr.s_addr = ip;
	if (!TryConnect(s, &sa, 6))
	{
		LogC(C_FAIL, L"TCP connect :80 FAILED");
		closesocket(s);
		return;
	}
	LogC(C_OK, L"TCP connect :80 OK");

	{ // minimal HTTP GET to prove data flows
		char req[200], buf[512];
		int rl = 0, n = -1;
		fd_set rf;
		struct timeval tv;
		int r;
		const char *g = "GET ";
		while (*g)
			req[rl++] = *g++;
		{
			int i;
			for (i = 0; pathA[i] && rl < 150; i++)
				req[rl++] = pathA[i];
		}
		{
			const char *h = " HTTP/1.0\r\nHost: ";
			while (*h)
				req[rl++] = *h++;
		}
		{
			int i;
			for (i = 0; hostA[i] && rl < 180; i++)
				req[rl++] = hostA[i];
		}
		{
			const char *e = "\r\nConnection: close\r\n\r\n";
			while (*e)
				req[rl++] = *e++;
		}
		send(s, req, rl, 0);
		// s is still NON-BLOCKING (from TryConnect) - select() for readability before recv,
		// else recv returns WSAEWOULDBLOCK instantly (looks like "no reply" but data's en route).
		FD_ZERO(&rf);
		FD_SET(s, &rf);
		tv.tv_sec = 6;
		tv.tv_usec = 0;
		r = select(0, &rf, 0, 0, &tv);
		if (r > 0 && FD_ISSET(s, &rf))
			n = recv(s, buf, sizeof(buf) - 1, 0);
		if (n > 0)
		{
			WCHAR line[58];
			int i, j = 0;
			buf[n] = 0; // show the HTTP status line
			for (i = 0; i < n && j < 54 && buf[i] != '\r' && buf[i] != '\n'; i++)
				line[j++] = (WCHAR)(unsigned char)buf[i];
			line[j] = 0;
			LogC(C_OK, line);
			LogF(C_INFO, L"(%u bytes received)", (DWORD)n);
		}
		else if (n == 0)
			LogC(C_FAIL, L"connected but server sent no data");
		else
			LogC(C_FAIL, L"no HTTP reply (6s timeout)");
	}
	closesocket(s);
}

// ---- download-to-RAM test: GET the whole body into a RAM buffer, verify the byte count, show
// progress. The buffer lives only in RAM and is freed when the app closes or a new Get starts
// (the DC filesystem is read-only anyway). Streams across frames so the UI + progress bar update.
enum
{
	DL_IDLE,
	DL_ACTIVE,
	DL_DONE,
	DL_FAIL
};
static int g_dl = DL_IDLE;
static SOCKET g_dlSock = INVALID_SOCKET;
static BYTE *g_dlBuf;    // RAM "file" (grown as data arrives, capped at DL_MAX)
static DWORD g_dlCap;    // allocated bytes
static DWORD g_dlStored; // bytes actually held in g_dlBuf (<= DL_MAX)
static DWORD g_dlRaw;    // total bytes received (headers+body), even past the cap
static int g_dlHdrDone;
static DWORD g_dlBodyOff;        // offset of the body within the response stream
static DWORD g_dlTotal;          // Content-Length (0 = unknown)
static DWORD g_dlLastData;       // tick of last recv (stall timeout)
static DWORD g_dlStart, g_dlEnd; // ticks: download begin / finish (for speed + elapsed)
#define DL_INIT (64 * 1024)
#define DL_MAX  (4 * 1024 * 1024)

// Format a rate (in KB/s) as "123 KB/s" or "1.2 MB/s" (integer math, no float).
static void SpeedStr(DWORD kbps, WCHAR *out)
{
	if (kbps >= 1024)
	{
		DWORD m10 = kbps * 10 / 1024;
		wsprintfW(out, L"%u.%u MB/s", m10 / 10, m10 % 10);
	}
	else
		wsprintfW(out, L"%u KB/s", kbps);
}

// Average rate so far, in KB/s (elapsed = now while active, else the finished span).
static DWORD DownloadKbps(DWORD body)
{
	DWORD el = (g_dl == DL_ACTIVE) ? (GetTickCount() - g_dlStart)
	                               : (g_dlEnd >= g_dlStart ? g_dlEnd - g_dlStart : 0);
	return el ? (body / 1024) * 1000 / el : 0; // (KB) * 1000ms / ms = KB/s, overflow-safe
}

static void DownloadFree(void)
{
	if (g_dlSock != INVALID_SOCKET)
	{
		closesocket(g_dlSock);
		g_dlSock = INVALID_SOCKET;
	}
	if (g_dlBuf)
	{
		LocalFree(g_dlBuf);
		g_dlBuf = 0;
	}
	g_dlCap = g_dlStored = g_dlRaw = g_dlBodyOff = g_dlTotal = 0;
	g_dlStart = g_dlEnd = 0;
	g_dlHdrDone = 0;
}

static DWORD DownloadBody(void)
{
	return (g_dlRaw >= g_dlBodyOff) ? g_dlRaw - g_dlBodyOff : 0;
}

// Once the header block is buffered, find the body offset (\r\n\r\n) and Content-Length.
static void DownloadParseHeaders(void)
{
	DWORD i;
	if (g_dlHdrDone || g_dlStored < 4)
		return;
	for (i = 0; i + 3 < g_dlStored; i++)
		if (g_dlBuf[i] == '\r' && g_dlBuf[i + 1] == '\n' && g_dlBuf[i + 2] == '\r' &&
		    g_dlBuf[i + 3] == '\n')
		{
			g_dlBodyOff = i + 4;
			g_dlHdrDone = 1;
			break;
		}
	if (!g_dlHdrDone)
		return;
	for (i = 0; i + 16 < g_dlBodyOff; i++) // case-insensitive "content-length:"
	{
		static const char cl[] = "content-length:";
		int j;
		char ch;
		for (j = 0; cl[j]; j++)
		{
			ch = (char)g_dlBuf[i + j];
			if (ch >= 'A' && ch <= 'Z')
				ch = (char)(ch + 32);
			if (ch != cl[j])
				break;
		}
		if (!cl[j])
		{
			DWORD v = 0, k = i + 15;
			while (k < g_dlBodyOff && (g_dlBuf[k] == ' ' || g_dlBuf[k] == '\t'))
				k++;
			while (k < g_dlBodyOff && g_dlBuf[k] >= '0' && g_dlBuf[k] <= '9')
				v = v * 10 + (g_dlBuf[k++] - '0');
			g_dlTotal = v;
			break;
		}
	}
}

// Append a recv'd chunk: count it (always) and store it into the RAM buffer up to DL_MAX (manual
// grow: alloc bigger + copy + free, avoiding LocalReAlloc handle semantics).
static void DownloadAppend(const char *data, int n)
{
	g_dlRaw += (DWORD)n;
	if (g_dlStored < DL_MAX)
	{
		DWORD need = g_dlStored + (DWORD)n;
		if (need > DL_MAX)
			need = DL_MAX;
		if (need > g_dlCap)
		{
			DWORD nc = g_dlCap ? g_dlCap : DL_INIT;
			BYTE *nb;
			while (nc < need)
				nc *= 2;
			if (nc > DL_MAX)
				nc = DL_MAX;
			nb = (BYTE *)LocalAlloc(LPTR, nc);
			if (nb)
			{
				if (g_dlBuf)
				{
					memcpy(nb, g_dlBuf, g_dlStored);
					LocalFree(g_dlBuf);
				}
				g_dlBuf = nb;
				g_dlCap = nc;
			}
		}
		if (g_dlBuf)
		{
			DWORD room = (g_dlCap > g_dlStored) ? g_dlCap - g_dlStored : 0;
			DWORD cpy = ((DWORD)n < room) ? (DWORD)n : room;
			memcpy(g_dlBuf + g_dlStored, data, cpy);
			g_dlStored += cpy;
		}
	}
	if (!g_dlHdrDone)
		DownloadParseHeaders();
}

static void DownloadFinish(void)
{
	DWORD body = DownloadBody(), el, kbps;
	if (g_dlSock != INVALID_SOCKET)
	{
		closesocket(g_dlSock);
		g_dlSock = INVALID_SOCKET;
	}
	g_dlEnd = GetTickCount();
	g_dl = DL_DONE;
	LogF(C_INFO, L"received %u body bytes", body);
	el = (g_dlEnd >= g_dlStart) ? g_dlEnd - g_dlStart : 0;
	kbps = DownloadKbps(body);
	{
		WCHAR sp[24], b[58];
		SpeedStr(kbps, sp);
		wsprintfW(b, L"%u.%us  avg %s", el / 1000, (el % 1000) / 100, sp);
		LogC(C_INFO, b);
	}
	if (g_dlTotal)
	{
		WCHAR b[58];
		if (body == g_dlTotal)
		{
			wsprintfW(b, L"COMPLETE: all %u bytes", g_dlTotal);
			LogC(C_OK, b);
		}
		else
		{
			wsprintfW(b, L"INCOMPLETE %u / %u bytes", body, g_dlTotal);
			LogC(C_FAIL, b);
		}
	}
	else
		LogC(C_OK, L"done (server sent no Content-Length)");
	if (g_dlRaw > g_dlStored)
		LogF(C_MUTE, L"(buffered first %u in RAM)", g_dlStored);
}

// Start a download of g_target (host or host/path) into RAM. Non-blocking; PollDownload finishes
// it.
static void DoDownloadStart(void)
{
	char hostA[80], pathA[96], req[220];
	struct hostent *he;
	unsigned long ip;
	SOCKET s;
	SOCKADDR_IN sa;
	const char *p;
	int rl = 0, i;

	if (g_dl == DL_ACTIVE)
		return;
	DownloadFree(); // drop the previous RAM file
	g_nLog = 0;
	SplitTarget(hostA, sizeof(hostA), pathA, sizeof(pathA));

	ip = inet_addr(hostA);
	if (ip == INADDR_NONE)
	{
		__try
		{
			he = gethostbyname(hostA);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			he = 0;
		}
		if (!he || !he->h_addr_list[0])
		{
			LogC(C_FAIL, L"DNS resolve FAILED");
			g_dl = DL_FAIL;
			return;
		}
		ip = *(unsigned long *)he->h_addr_list[0];
	}
	{
		WCHAR b[58], ips[24];
		IpToStr(ip, ips);
		wsprintfW(b, L"GET from %s", ips);
		LogC(C_INFO, b);
	}

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
	{
		LogC(C_FAIL, L"socket() failed");
		g_dl = DL_FAIL;
		return;
	}
	{
		int rb = 32 * 1024;
		setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&rb, sizeof(rb));
	} // bigger RX window
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	sa.sin_addr.s_addr = ip;
	if (!TryConnect(s, &sa, 6))
	{
		LogC(C_FAIL, L"TCP connect :80 FAILED");
		closesocket(s);
		g_dl = DL_FAIL;
		return;
	}
	LogC(C_OK, L"connected - downloading...");

	p = "GET ";
	while (*p)
		req[rl++] = *p++;
	for (i = 0; pathA[i] && rl < 160; i++)
		req[rl++] = pathA[i];
	p = " HTTP/1.0\r\nHost: ";
	while (*p)
		req[rl++] = *p++;
	for (i = 0; hostA[i] && rl < 200; i++)
		req[rl++] = hostA[i];
	p = "\r\nConnection: close\r\n\r\n";
	while (*p)
		req[rl++] = *p++;
	send(s, req, rl, 0); // socket is non-blocking (TryConnect set FIONBIO)

	g_dlSock = s;
	g_dl = DL_ACTIVE;
	g_dlStart = g_dlLastData = GetTickCount();
}

// Drive the active download: drain readable bytes (budgeted per frame so the UI stays live).
// recv==0 means the server closed (Connection: close) = complete. Returns 1 to force a redraw.
static int PollDownload(void)
{
	char tmp[8192];
	int n;
	DWORD budget = 0;
	if (g_dl != DL_ACTIVE)
		return 0;
	for (;;) // drain readable bytes; the socket is non-blocking
	{
		n = recv(g_dlSock, tmp, sizeof(tmp), 0); // recv directly (no select readability poll)
		if (n == 0)
		{
			DownloadFinish();
			return 1;
		}
		if (n < 0)
		{
			if (WSAGetLastError() == WSAEWOULDBLOCK)
				break;
			LogC(C_FAIL, L"recv error");
			g_dl = DL_FAIL;
			g_dlEnd = GetTickCount();
			closesocket(g_dlSock);
			g_dlSock = INVALID_SOCKET;
			return 1;
		}
		DownloadAppend(tmp, n);
		g_dlLastData = GetTickCount();
		if (g_dlTotal && DownloadBody() >= g_dlTotal)
		{
			DownloadFinish();
			return 1;
		} // got it all
		budget += (DWORD)n;
		if (budget >= 262144)
			break; // yield to the UI; resume next frame
	}
	if (GetTickCount() - g_dlLastData > 15000)
	{
		LogC(C_FAIL, L"download stalled (15s)");
		g_dl = DL_FAIL;
		g_dlEnd = GetTickCount();
		if (g_dlSock != INVALID_SOCKET)
		{
			closesocket(g_dlSock);
			g_dlSock = INVALID_SOCKET;
		}
		return 1;
	}
	return 1; // active -> redraw progress each frame
}

// ---- on-screen keyboard + buttons (clickable via the analog-stick pointer) ----
static const WCHAR *kbRows[4] = {L"1234567890", L"qwertyuiop", L"asdfghjkl-", L"zxcvbnm.:/"};
#define KCOLS 10
#define KW    30
#define KH    22
#define KOX   8
#define KOY   76
#define KGAP  2

// action buttons: id, label, x, w (all on one row at BTN_Y)
#define BTN_Y 196
enum
{
	B_SPACE,
	B_DOTCOM,
	B_BKSP,
	B_CLEAR,
	B_DIAL,
	B_TEST,
	B_GET,
	B_HANG,
	B_COUNT
};
typedef struct
{
	int x, w;
	const WCHAR *label;
	COLORREF c;
} Btn;
static const Btn s_btn[B_COUNT] = {
    {8, 48, L"space", C_KEY},   {60, 48, L".com", C_KEY},    {112, 44, L"Bksp", C_KEY},
    {160, 48, L"Clear", C_KEY}, {222, 52, L"Dial", C_BTN},   {278, 44, L"Test", C_BTN},
    {326, 44, L"Get", C_BTN},   {374, 86, L"HangUp", C_BTN},
};

static void EditAppendCh(WCHAR ch)
{
	int n = lstrlenW(g_target);
	if (n < 78)
	{
		g_target[n] = ch;
		g_target[n + 1] = 0;
	}
}
static void EditAppendStr(const WCHAR *s)
{
	int n = lstrlenW(g_target), i;
	for (i = 0; s[i] && n < 78; i++)
		g_target[n++] = s[i];
	g_target[n] = 0;
}
static void EditBksp(void)
{
	int n = lstrlenW(g_target);
	if (n)
		g_target[n - 1] = 0;
}

// Hit-test a click at client (x,y). Returns 1 if it changed something needing a redraw.
static int HandleClick(int x, int y)
{
	int r, c, i;
	for (r = 0; r < 4; r++) // OSK char keys
		for (c = 0; c < KCOLS; c++)
		{
			int kx = KOX + c * (KW + KGAP), ky = KOY + r * (KH + KGAP);
			if (x >= kx && x < kx + KW && y >= ky && y < ky + KH)
			{
				EditAppendCh(kbRows[r][c]);
				return 1;
			}
		}
	for (i = 0; i < B_COUNT; i++) // action buttons
	{
		if (x >= s_btn[i].x && x < s_btn[i].x + s_btn[i].w && y >= BTN_Y && y < BTN_Y + KH)
		{
			switch (i)
			{
				case B_SPACE:
					EditAppendCh(L' ');
					break;
				case B_DOTCOM:
					EditAppendStr(L".com");
					break;
				case B_BKSP:
					EditBksp();
					break;
				case B_CLEAR:
					g_target[0] = 0;
					break;
				case B_DIAL:
					DoDial();
					break;
				case B_TEST:
					DoTest();
					break;
				case B_GET:
					DoDownloadStart();
					break;
				case B_HANG:
					DoHangup();
					break;
			}
			return 1;
		}
	}
	return 0;
}

static const WCHAR *ConnText(void)
{
	switch (g_conn)
	{
		case CN_DIALING:
			return L"dialing...";
		case CN_UP:
			return L"CONNECTED";
		case CN_FAILED:
			return L"failed";
		default:
			return L"idle";
	}
}

static void Draw(DCWin *w, int cw, int ch)
{
	int r, c, i, y;
	WCHAR line[64];

	DCWinFillBg(w, C_BG);
	DCWinFill(w, 6, 4, cw - 12, 16, C_HDR);
	DCWinText(w, 10, 5, C_WHITE, C_HDR, L"Network Diagnostics");

	// status: connection / IP / DNS
	wsprintfW(line, L"Conn: %s    IP: %s    DNS: %s", ConnText(), g_ipStr, g_dnsStr);
	DCWinText(w, 8, 24, (g_conn == CN_UP) ? C_OK : (g_conn == CN_FAILED ? C_FAIL : C_BLACK), C_BG,
	          line);

	// target field
	DCWinText(w, 8, 44, C_BLACK, C_BG, L"Target:");
	DCWinFill(w, 58, 42, cw - 66, 18, C_WHITE);
	DCWinText(w, 62, 44, C_BLACK, C_WHITE, g_target[0] ? g_target : L"(type a host or IP)");

	// OSK
	for (r = 0; r < 4; r++)
		for (c = 0; c < KCOLS; c++)
		{
			WCHAR k[2];
			int kx = KOX + c * (KW + KGAP), ky = KOY + r * (KH + KGAP);
			k[0] = kbRows[r][c];
			k[1] = 0;
			DCWinFill(w, kx, ky, KW, KH, C_KEYF);
			DCWinText(w, kx + 11, ky + 3, C_BLACK, C_KEYF, k);
		}
	// buttons
	for (i = 0; i < B_COUNT; i++)
	{
		COLORREF bg = s_btn[i].c, fg = (bg == C_BTN) ? C_WHITE : C_BLACK;
		DCWinFill(w, s_btn[i].x, BTN_Y, s_btn[i].w, KH, bg);
		DCWinText(w, s_btn[i].x + 5, BTN_Y + 3, fg, bg, s_btn[i].label);
	}

	// download progress bar (shown once a Get has run)
	if (g_dl != DL_IDLE)
	{
		DWORD body = DownloadBody();
		DWORD pct = g_dlTotal ? (body / 1024 * 100) / (g_dlTotal / 1024 + 1) : 0;
		int barw = cw - 16, fill;
		COLORREF pc = (g_dl == DL_FAIL) ? C_FAIL : (g_dl == DL_DONE ? C_OK : C_HDR);
		const WCHAR *st = (g_dl == DL_ACTIVE) ? L"Downloading"
		                  : (g_dl == DL_DONE) ? L"Downloaded"
		                                      : L"Download failed";
		WCHAR pl[96], sp[24];
		SpeedStr(DownloadKbps(body), sp);
		if (g_dl == DL_DONE && g_dlTotal && body >= g_dlTotal)
			pct = 100;
		fill = g_dlTotal ? (int)((DWORD)barw * pct / 100) : (g_dl == DL_DONE ? barw : 0);
		if (g_dlTotal)
			wsprintfW(pl, L"%s  %u / %u bytes (%u%%)  %s", st, body, g_dlTotal, pct, sp);
		else
			wsprintfW(pl, L"%s  %u bytes  %s", st, body, sp);
		DCWinText(w, 8, 224, C_BLACK, C_BG, pl);
		DCWinFill(w, 8, 238, barw, 12, C_WHITE); // track
		if (fill > 0)
			DCWinFill(w, 8, 238, fill, 12, pc); // fill
	}

	// result log
	DCWinFill(w, 6, 256, cw - 12, 1, C_MUTE);
	for (i = 0, y = 262; i < g_nLog; i++, y += 15)
		DCWinText(w, 10, y, g_log[i].c, C_BG, g_log[i].s);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
	DCWin *w;
	WSADATA wsa;
	int cw = CW, ch = CH, dirty = 1, prevBtn = 0;
	DWORD key;

	w = DCWinOpen(70, 40, CW, CH, L"Network Diagnostics", ICON_APP);
	if (!w)
	{
		OutputDebugStringW(L"DCWNET: DCWinOpen failed\r\n");
		return 1;
	}
	WSAStartup(MAKEWORD(1, 1), &wsa);
	RefreshLocalIp();
	LogC(C_INFO, L"Point + A on Dial, then Test.");

	for (;;)
	{
		int px, py, btn;
		if (DCWinClientSize(w, &cw, &ch))
			dirty = 1;

		while (DCWinPollKey(w, &key)) // keyboard edits (NOT Enter: the shell
		{                             // synthesizes VK_RETURN on every body click,
			if (key == VK_BACK)
			{
				EditBksp();
				dirty = 1;
			} // so binding it to a connect made
		} // every OSK keypress fire a blocking Test)

		if (DCWinGetPointer(w, &px, &py, &btn)) // analog-stick cursor over our window
		{
			if (btn && !prevBtn)
			{
				if (HandleClick(px, py))
					dirty = 1;
			} // click edge
			prevBtn = btn;
		}
		else
			prevBtn = 0;

		if (PollDial())
			dirty = 1; // advance a modem dial in progress
		if (PollDownload())
			dirty = 1; // drain an in-progress download

		if (dirty)
		{
			DCWinBeginFrame(w);
			Draw(w, cw, ch);
			DCWinEndFrame(w);
			dirty = 0;
		}
		if (g_conn == CN_DIALING)
			dirty = 1; // keep polling/redrawing while dialing
		if (g_dl == DL_ACTIVE)
			dirty = 1; // keep draining/redrawing while downloading
		if (DCWinShouldClose(w))
			break;
		Sleep(20);
	}

	DownloadFree(); // the RAM "file" is destroyed on close
	if (g_hConn)
		RasHangUp(g_hConn);
	WSACleanup();
	DCWinClose(w);
	return 0;
}
