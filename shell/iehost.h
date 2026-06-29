//
// iehost.h - DCWin Internet Explorer: the WebBrowser-control host object.
//
// CBrowserHost embeds the stock Trident WebBrowser control (CLSID_WebBrowser,
// shdocvw.dll -> mshtml.dll) as an in-place OLE/ActiveX object, exactly the way
// the Sega "Dragon" SDK htmlsamp does. It implements the minimal container site
// (IOleClientSite / IOleInPlaceSite / IOleContainer / IOleCommandTarget) plus a
// DWebBrowserEvents2 sink, and drives navigation through IWebBrowser2.
//
// The control renders itself via GWES into a child window of the frame; we never
// composite its pixels. The app (iexplore.cpp) owns the frame window + chrome and
// calls into this object. Navigation/title/status feed back through the Ui* hooks
// below, which iexplore.cpp implements to repaint the address/status bars.
//
#ifndef IEHOST_H
#define IEHOST_H

#include <windows.h>

#ifdef __cplusplus

#include <objbase.h>
#include <oleidl.h>
#include <docobj.h>      // IOleCommandTarget, OLECMD*
#include <exdisp.h>      // IWebBrowser2, DWebBrowserEvents2
#include <exdispid.h>    // DISPID_* event ids

struct IHTMLWindow2;     // mshtml.h (full def only needed in iehost.cpp, for ScrollBy)

// One concrete container site multiply-inheriting every interface the control
// asks its host for. AddRef/Release/QueryInterface are overridden once; the COM
// MI rule (cast `this` to the requested base in QI) keeps the vtables straight.
class CBrowserHost : public IOleClientSite,
                     public IOleInPlaceSite,
                     public IOleContainer,
                     public IOleCommandTarget,
                     public DWebBrowserEvents2
{
public:
    CBrowserHost(HWND hwndFrame, const RECT *prc);
    ~CBrowserHost(void);

    // --- app-facing API ---------------------------------------------------
    BOOL    Create(void);                       // CoCreate + in-place activate + event sink
    void    Destroy(void);
    HWND    GetControlWindow(void) const { return _hwnd; }
    void    SetRect(const RECT *prc);           // relayout the control (resize)
    HRESULT Navigate(LPCWSTR url);        // (WinInet path - unused on DC; kept for about:blank)
    HRESULT LoadUrl(LPCWSTR url);         // fetch via winsock and render the bytes into the control
    void    PumpPending(void);            // service a link the BeforeNavigate2 hook captured+cancelled
    HRESULT GoBack(void);
    HRESULT GoForward(void);
    HRESULT Refresh(void);
    HRESULT Stop(void);
    HRESULT ScrollBy(int dx, int dy);           // scroll the document via IHTMLWindow2
    HRESULT TranslateAccel(MSG *pMsg);          // give the control first crack at a key msg

    // --- IUnknown ---------------------------------------------------------
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv);
    STDMETHOD_(ULONG, AddRef)(void);
    STDMETHOD_(ULONG, Release)(void);

    // --- IOleClientSite ---------------------------------------------------
    STDMETHOD(SaveObject)(void);
    STDMETHOD(GetMoniker)(DWORD, DWORD, IMoniker **);
    STDMETHOD(GetContainer)(IOleContainer **);
    STDMETHOD(ShowObject)(void);
    STDMETHOD(OnShowWindow)(BOOL);
    STDMETHOD(RequestNewObjectLayout)(void);

    // --- IOleWindow (base of IOleInPlaceSite) -----------------------------
    STDMETHOD(GetWindow)(HWND *);
    STDMETHOD(ContextSensitiveHelp)(BOOL);

    // --- IOleInPlaceSite --------------------------------------------------
    STDMETHOD(CanInPlaceActivate)(void);
    STDMETHOD(OnInPlaceActivate)(void);
    STDMETHOD(OnUIActivate)(void);
    STDMETHOD(GetWindowContext)(IOleInPlaceFrame **, IOleInPlaceUIWindow **,
                                LPRECT, LPRECT, LPOLEINPLACEFRAMEINFO);
    STDMETHOD(Scroll)(SIZE);
    STDMETHOD(OnUIDeactivate)(BOOL);
    STDMETHOD(OnInPlaceDeactivate)(void);
    STDMETHOD(DiscardUndoState)(void);
    STDMETHOD(DeactivateAndUndo)(void);
    STDMETHOD(OnPosRectChange)(LPCRECT);

    // --- IOleContainer ----------------------------------------------------
    STDMETHOD(ParseDisplayName)(IBindCtx *, LPOLESTR, ULONG *, IMoniker **);
    STDMETHOD(EnumObjects)(DWORD, IEnumUnknown **);
    STDMETHOD(LockContainer)(BOOL);

    // --- IOleCommandTarget ------------------------------------------------
    STDMETHOD(QueryStatus)(const GUID *, ULONG, OLECMD *, OLECMDTEXT *);
    STDMETHOD(Exec)(const GUID *, DWORD, DWORD, VARIANTARG *, VARIANTARG *);

    // --- IDispatch (DWebBrowserEvents2 sink) -------------------------------
    STDMETHOD(GetTypeInfoCount)(UINT *);
    STDMETHOD(GetTypeInfo)(UINT, LCID, ITypeInfo **);
    STDMETHOD(GetIDsOfNames)(REFIID, OLECHAR **, UINT, LCID, DISPID *);
    STDMETHOD(Invoke)(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *,
                      EXCEPINFO *, UINT *);

private:
    HRESULT InitEvents(void);
    HRESULT GetHtmlWindow(IHTMLWindow2 **ppWin);

    ULONG                    _refs;
    HWND                     _frame;     // our top-level frame window (parent of the control)
    HWND                     _hwnd;      // the control's window (from IOleWindow::GetWindow)
    RECT                     _rc;        // control rect within the frame
    IWebBrowser2            *_pWB2;
    IOleObject              *_pOle;
    IOleInPlaceObject       *_pIPO;
    IOleInPlaceActiveObject *_pIPAO;
    IConnectionPoint        *_pCP;
    DWORD                    _cookie;
};

#endif // __cplusplus

// --- UI hooks: implemented by iexplore.cpp, called from the event sink ----
#ifdef __cplusplus
extern "C" {
#endif
void UiOnNavigate(const WCHAR *url);     // BEFORENAVIGATE2 / NAVIGATECOMPLETE2 -> new address
void UiOnTitle(const WCHAR *title);      // TITLECHANGE
void UiOnStatus(const WCHAR *status);    // STATUSTEXTCHANGE / progress text
void UiOnBusy(int busy);                 // DOWNLOADBEGIN(1) / DOCUMENTCOMPLETE(0)
void UiOnSecure(int secure);             // SECURITYICONCHANGE
#ifdef __cplusplus
}
#endif

#endif // IEHOST_H
