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
#define CH 348

// ---- colours ----
#define C_BG     RGB(192, 192, 192)
#define C_HDR    RGB(0, 0, 128)
#define C_WHITE  RGB(255, 255, 255)
#define C_BLACK  RGB(0, 0, 0)
#define C_KEY    RGB(176, 176, 176)
#define C_KEYF   RGB(224, 224, 224)
#define C_BTN    RGB(0, 0, 128)
#define C_OK     RGB(0, 112, 0)
#define C_FAIL   RGB(176, 0, 0)
#define C_INFO   RGB(0, 0, 160)
#define C_MUTE   RGB(96, 96, 96)

// ---- target host/IP being edited ----
static WCHAR g_target[80] = L"www.sega.com";

// ---- connection state ----
enum { CN_IDLE, CN_DIALING, CN_UP, CN_FAILED };
static int       g_conn = CN_IDLE;
static HRASCONN  g_hConn;
static DWORD     g_dialStart;
static WCHAR     g_ipStr[24]  = L"-";
static WCHAR     g_dnsStr[24] = L"-";

// ---- colour-coded result log ----
typedef struct { COLORREF c; WCHAR s[58]; } LogLine;
static LogLine g_log[7];
static int     g_nLog;

static void LogC(COLORREF c, const WCHAR *s)
{
    int i;
    if (g_nLog >= 7) { for (i = 0; i < 6; i++) g_log[i] = g_log[i + 1]; g_nLog = 6; }  // scroll
    g_log[g_nLog].c = c;
    for (i = 0; i < 56 && s[i]; i++) g_log[g_nLog].s[i] = s[i];
    g_log[g_nLog].s[i] = 0;
    g_nLog++;
}
static void LogF(COLORREF c, const WCHAR *fmt, DWORD a) { WCHAR b[58]; wsprintfW(b, fmt, a); LogC(c, b); }

// ---- registry DNS (the shim writes HKLM\Comm "DnsServers" = [count][ip...] net order) ----
static unsigned long ReadDns(void)
{
    HKEY h; DWORD t, n; unsigned long buf[6], ip = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Comm", 0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS)
    {
        n = sizeof(buf);
        if (RegQueryValueExW(h, L"DnsServers", 0, &t, (BYTE *)buf, &n) == ERROR_SUCCESS && n >= 8 && buf[0] >= 1)
            ip = buf[1];
        RegCloseKey(h);
    }
    return ip;
}

static void IpToStr(unsigned long ip, WCHAR *out)
{ unsigned char *p = (unsigned char *)&ip; wsprintfW(out, L"%u.%u.%u.%u", p[0], p[1], p[2], p[3]); }

// Local bound address: a UDP socket "connected" to the DNS server (no packets sent) lets
// getsockname report the source IP the stack would use - works on ethernet and PPP alike.
static void RefreshLocalIp(void)
{
    unsigned long dns = ReadDns();
    SOCKET s;
    SOCKADDR_IN sa, me; int ml = sizeof(me);
    if (dns) IpToStr(dns, g_dnsStr); else lstrcpyW(g_dnsStr, L"(none)");
    lstrcpyW(g_ipStr, L"-");
    if (!dns) return;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons(53); sa.sin_addr.s_addr = dns;
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
    lstrcpyW(re.szLocalPhoneNumber, L"0118999");   // Flycast/null-modem ignore the number
    RasSetEntryProperties(NULL, L"DC Modem", &re, sizeof(re), NULL, 0);
}

static void DoDial(void)
{
    RASDIALPARAMS p;
    DWORD rc;
    if (g_conn == CN_DIALING || g_conn == CN_UP) return;
    g_nLog = 0;
    LogC(C_INFO, L"Dialing (RasDial)...");
    EnsureEntry();
    memset(&p, 0, sizeof(p));
    p.dwSize = sizeof(p);
    lstrcpyW(p.szEntryName, L"DC Modem");
    g_hConn = 0;
    rc = RasDial(NULL, NULL, &p, 0, NULL, &g_hConn);
    if (rc != 0) { LogF(C_FAIL, L"RasDial err %u", rc); g_conn = CN_FAILED; return; }
    g_conn = CN_DIALING;
    g_dialStart = GetTickCount();
}

// Non-blocking dial progress; called each frame while CN_DIALING. Returns 1 if state changed.
static int PollDial(void)
{
    RASCONNSTATUS st;
    if (g_conn != CN_DIALING) return 0;
    memset(&st, 0, sizeof(st)); st.dwSize = sizeof(st);
    RasGetConnectStatus(g_hConn, &st);
    if (st.rasconnstate == RASCS_Connected)
    {
        WCHAR b[58];
        g_conn = CN_UP;
        RefreshLocalIp();
        LogC(C_OK, L"Connected.");
        wsprintfW(b, L"  IP  %s", g_ipStr);  LogC(C_INFO, b);
        wsprintfW(b, L"  DNS %s", g_dnsStr); LogC(C_INFO, b);
        return 1;
    }
    if (st.dwError != 0) { LogF(C_FAIL, L"Dial failed, err %u", st.dwError); g_conn = CN_FAILED; return 1; }
    if (GetTickCount() - g_dialStart > 45000) { LogC(C_FAIL, L"Dial timeout (45s)"); g_conn = CN_FAILED; return 1; }
    return 0;
}

static void DoHangup(void)
{
    if (g_hConn) RasHangUp(g_hConn);
    g_hConn = 0; g_conn = CN_IDLE;
    lstrcpyW(g_ipStr, L"-");
    LogC(C_MUTE, L"Hung up.");
}

// ---- the actual reachability test (DNS -> TCP -> HTTP), on g_target ----
static int TryConnect(SOCKET s, SOCKADDR_IN *sa, int secs)
{
    unsigned long nb = 1; struct timeval tv; fd_set wf, ef; int r;
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (SOCKADDR *)sa, sizeof(*sa)) == 0) return 1;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return 0;
    FD_ZERO(&wf); FD_SET(s, &wf); FD_ZERO(&ef); FD_SET(s, &ef);
    tv.tv_sec = secs; tv.tv_usec = 0;
    r = select(0, 0, &wf, &ef, &tv);
    return (r > 0 && FD_ISSET(s, &wf)) ? 1 : 0;
}

static void DoTest(void)
{
    char  hostA[80];
    struct hostent *he;
    unsigned long ip;
    SOCKET s;
    SOCKADDR_IN sa;
    int isIp;

    g_nLog = 0;
    if (g_conn != CN_UP) LogC(C_MUTE, L"(not dialed - testing anyway)");
    WideCharToMultiByte(CP_ACP, 0, g_target, -1, hostA, sizeof(hostA), 0, 0);

    // numeric IP or hostname?
    ip = inet_addr(hostA);
    isIp = (ip != INADDR_NONE);
    if (isIp) { WCHAR b[40]; wsprintfW(b, L"target is literal IP"); LogC(C_INFO, b); }
    else
    {
        __try { he = gethostbyname(hostA); }
        __except (EXCEPTION_EXECUTE_HANDLER) { he = 0; }
        if (he && he->h_addr_list[0])
        {
            WCHAR b[58], ips[24]; ip = *(unsigned long *)he->h_addr_list[0];
            IpToStr(ip, ips); wsprintfW(b, L"DNS OK -> %s", ips); LogC(C_OK, b);
        }
        else { LogC(C_FAIL, L"DNS resolve FAILED"); return; }
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { LogC(C_FAIL, L"socket() failed"); return; }
    memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons(80); sa.sin_addr.s_addr = ip;
    if (!TryConnect(s, &sa, 6)) { LogC(C_FAIL, L"TCP connect :80 FAILED"); closesocket(s); return; }
    LogC(C_OK, L"TCP connect :80 OK");

    {   // minimal HTTP GET to prove data flows
        char req[160], buf[512]; int rl = 0, n = -1;
        fd_set rf; struct timeval tv; int r;
        const char *g = "GET / HTTP/1.0\r\nHost: ";
        while (*g) req[rl++] = *g++;
        { int i; for (i = 0; hostA[i] && rl < 150; i++) req[rl++] = hostA[i]; }
        { const char *e = "\r\nConnection: close\r\n\r\n"; while (*e) req[rl++] = *e++; }
        send(s, req, rl, 0);
        // s is still NON-BLOCKING (from TryConnect) - select() for readability before recv,
        // else recv returns WSAEWOULDBLOCK instantly (looks like "no reply" but data's en route).
        FD_ZERO(&rf); FD_SET(s, &rf); tv.tv_sec = 6; tv.tv_usec = 0;
        r = select(0, &rf, 0, 0, &tv);
        if (r > 0 && FD_ISSET(s, &rf)) n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            WCHAR line[58]; int i, j = 0; buf[n] = 0;          // show the HTTP status line
            for (i = 0; i < n && j < 54 && buf[i] != '\r' && buf[i] != '\n'; i++)
                line[j++] = (WCHAR)(unsigned char)buf[i];
            line[j] = 0;
            LogC(C_OK, line);
            LogF(C_INFO, L"(%u bytes received)", (DWORD)n);
        }
        else if (n == 0) LogC(C_FAIL, L"connected but server sent no data");
        else             LogC(C_FAIL, L"no HTTP reply (6s timeout)");
    }
    closesocket(s);
}

// ---- on-screen keyboard + buttons (clickable via the analog-stick pointer) ----
static const WCHAR *kbRows[4] = { L"1234567890", L"qwertyuiop", L"asdfghjkl-", L"zxcvbnm.:/" };
#define KCOLS 10
#define KW 30
#define KH 22
#define KOX 8
#define KOY 76
#define KGAP 2

// action buttons: id, label, x, w (all on one row at BTN_Y)
#define BTN_Y 196
enum { B_SPACE, B_DOTCOM, B_BKSP, B_CLEAR, B_DIAL, B_TEST, B_HANG, B_COUNT };
typedef struct { int x, w; const WCHAR *label; COLORREF c; } Btn;
static const Btn s_btn[B_COUNT] = {
    {   8, 48, L"space",  C_KEY  },
    {  60, 48, L".com",   C_KEY  },
    { 112, 44, L"Bksp",   C_KEY  },
    { 160, 48, L"Clear",  C_KEY  },
    { 264, 56, L"Dial",   C_BTN  },
    { 324, 56, L"Test",   C_BTN  },
    { 384, 76, L"HangUp", C_BTN  },
};

static void EditAppendCh(WCHAR ch)
{ int n = lstrlenW(g_target); if (n < 78) { g_target[n] = ch; g_target[n + 1] = 0; } }
static void EditAppendStr(const WCHAR *s)
{ int n = lstrlenW(g_target), i; for (i = 0; s[i] && n < 78; i++) g_target[n++] = s[i]; g_target[n] = 0; }
static void EditBksp(void) { int n = lstrlenW(g_target); if (n) g_target[n - 1] = 0; }

// Hit-test a click at client (x,y). Returns 1 if it changed something needing a redraw.
static int HandleClick(int x, int y)
{
    int r, c, i;
    for (r = 0; r < 4; r++)                          // OSK char keys
        for (c = 0; c < KCOLS; c++)
        {
            int kx = KOX + c * (KW + KGAP), ky = KOY + r * (KH + KGAP);
            if (x >= kx && x < kx + KW && y >= ky && y < ky + KH)
            { EditAppendCh(kbRows[r][c]); return 1; }
        }
    for (i = 0; i < B_COUNT; i++)                    // action buttons
    {
        if (x >= s_btn[i].x && x < s_btn[i].x + s_btn[i].w && y >= BTN_Y && y < BTN_Y + KH)
        {
            switch (i)
            {
            case B_SPACE:  EditAppendCh(L' ');      break;
            case B_DOTCOM: EditAppendStr(L".com");  break;
            case B_BKSP:   EditBksp();              break;
            case B_CLEAR:  g_target[0] = 0;         break;
            case B_DIAL:   DoDial();                break;
            case B_TEST:   DoTest();                break;
            case B_HANG:   DoHangup();              break;
            }
            return 1;
        }
    }
    return 0;
}

static const WCHAR *ConnText(void)
{
    switch (g_conn) {
    case CN_DIALING: return L"dialing...";
    case CN_UP:      return L"CONNECTED";
    case CN_FAILED:  return L"failed";
    default:         return L"idle";
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
    DCWinText(w, 8, 24, (g_conn == CN_UP) ? C_OK : (g_conn == CN_FAILED ? C_FAIL : C_BLACK), C_BG, line);

    // target field
    DCWinText(w, 8, 44, C_BLACK, C_BG, L"Target:");
    DCWinFill(w, 58, 42, cw - 66, 18, C_WHITE);
    DCWinText(w, 62, 44, C_BLACK, C_WHITE, g_target[0] ? g_target : L"(type a host or IP)");

    // OSK
    for (r = 0; r < 4; r++)
        for (c = 0; c < KCOLS; c++)
        {
            WCHAR k[2]; int kx = KOX + c * (KW + KGAP), ky = KOY + r * (KH + KGAP);
            k[0] = kbRows[r][c]; k[1] = 0;
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

    // result log
    DCWinFill(w, 6, BTN_Y + 28, cw - 12, 1, C_MUTE);
    for (i = 0, y = BTN_Y + 34; i < g_nLog; i++, y += 15)
        DCWinText(w, 10, y, g_log[i].c, C_BG, g_log[i].s);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    DCWin *w;
    WSADATA wsa;
    int  cw = CW, ch = CH, dirty = 1, prevBtn = 0;
    DWORD key;

    w = DCWinOpen(70, 40, CW, CH, L"Network Diagnostics", ICON_APP);
    if (!w) { OutputDebugStringW(L"DCWNET: DCWinOpen failed\r\n"); return 1; }
    WSAStartup(MAKEWORD(1, 1), &wsa);
    RefreshLocalIp();
    LogC(C_INFO, L"Point + A on Dial, then Test.");

    for (;;)
    {
        int px, py, btn;
        if (DCWinClientSize(w, &cw, &ch)) dirty = 1;

        while (DCWinPollKey(w, &key))                // keyboard shortcuts (optional)
        {
            if (key == VK_RETURN) { DoTest(); dirty = 1; }
            else if (key == VK_BACK) { EditBksp(); dirty = 1; }
        }

        if (DCWinGetPointer(w, &px, &py, &btn))      // analog-stick cursor over our window
        {
            if (btn && !prevBtn) { if (HandleClick(px, py)) dirty = 1; }   // click edge
            prevBtn = btn;
        }
        else prevBtn = 0;

        if (PollDial()) dirty = 1;                   // advance a modem dial in progress

        if (dirty) { DCWinBeginFrame(w); Draw(w, cw, ch); DCWinEndFrame(w); dirty = 0; }
        if (g_conn == CN_DIALING) dirty = 1;         // keep polling/redrawing while dialing
        if (DCWinShouldClose(w)) break;
        Sleep(20);
    }

    if (g_hConn) RasHangUp(g_hConn);
    WSACleanup();
    DCWinClose(w);
    return 0;
}
