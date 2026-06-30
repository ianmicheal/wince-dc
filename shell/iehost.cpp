//
// iehost.cpp - the WebBrowser-control host object (see iehost.h).
//
// Direct descendant of the Sega htmlsamp CHTMLDoc container, trimmed to the
// browser essentials: no game board, no RAS dialer, no IME - just host the
// control, drive IWebBrowser2, and surface navigation events to the chrome.
//
#include "iehost.h"
#include <mshtml.h> // IHTMLDocument2 / IHTMLWindow2 (document scroll)

extern "C" void UiOnNavigate(const WCHAR *url);  // (also in iehost.h, but keep visible here)
extern "C" void IeRegisterWinsockProtocol(void); // ieproto.cpp: override http -> winsock

// exdispid.h spells the secure-lock event differently across SDK drops.
#ifndef DISPID_SETSECURELOCKICON
#ifdef DISPID_SECURITYICONCHANGE
#define DISPID_SETSECURELOCKICON DISPID_SECURITYICONCHANGE
#endif
#endif

CBrowserHost::CBrowserHost(HWND hwndFrame, const RECT *prc)
{
	_refs = 1;
	_frame = hwndFrame;
	_hwnd = NULL;
	_rc = *prc;
	_pWB2 = NULL;
	_pOle = NULL;
	_pIPO = NULL;
	_pIPAO = NULL;
	_pCP = NULL;
	_cookie = 0;
}

CBrowserHost::~CBrowserHost(void)
{
	Destroy();
}

//
// Bring up the control: CoCreate -> set client site -> in-place UI-activate as a
// child of the frame -> grab IWebBrowser2 -> hook events -> get the HWND.
//
BOOL CBrowserHost::Create(void)
{
	IUnknown *pUnk = NULL;

	IeRegisterWinsockProtocol(); // route the control's http binds through winsock

	if (FAILED(CoCreateInstance(CLSID_WebBrowser, NULL,
	                            CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER, IID_IUnknown,
	                            (void **)&pUnk)) ||
	    !pUnk)
	{
		OutputDebugStringW(L"IE: CoCreate(CLSID_WebBrowser) FAILED\r\n");
		return FALSE;
	}

	if (SUCCEEDED(pUnk->QueryInterface(IID_IOleObject, (void **)&_pOle)))
	{
		DWORD dwMisc = 0;
		if (SUCCEEDED(_pOle->GetMiscStatus(DVASPECT_CONTENT, &dwMisc)) &&
		    (dwMisc & OLEMISC_SETCLIENTSITEFIRST))
		{
			_pOle->SetClientSite((IOleClientSite *)this);
		}

		// in-place UI activate inside the frame, sized to _rc
		if (SUCCEEDED(
		        _pOle->DoVerb(OLEIVERB_UIACTIVATE, NULL, (IOleClientSite *)this, 0, _frame, &_rc)))
		{
			_pOle->QueryInterface(IID_IOleInPlaceObject, (void **)&_pIPO);
			if (SUCCEEDED(_pOle->QueryInterface(IID_IWebBrowser2, (void **)&_pWB2)))
				InitEvents();
		}
		pUnk->QueryInterface(IID_IOleInPlaceActiveObject, (void **)&_pIPAO);
	}
	pUnk->Release();

	if (!_pWB2)
	{
		OutputDebugStringW(L"IE: control activation FAILED\r\n");
		return FALSE;
	}

	// the control's window (parent for keyboard focus)
	{
		IOleWindow *pOW = NULL;
		if (SUCCEEDED(_pWB2->QueryInterface(IID_IOleWindow, (void **)&pOW)))
		{
			pOW->GetWindow(&_hwnd);
			pOW->Release();
		}
	}
	if (_hwnd)
	{
		SetWindowLong(_hwnd, GWL_EXSTYLE, GetWindowLong(_hwnd, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
		SetFocus(_hwnd);
	}
	OutputDebugStringW(L"IE: WebBrowser control up\r\n");
	return TRUE;
}

void CBrowserHost::Destroy(void)
{
	if (_pCP && _cookie)
	{
		_pCP->Unadvise(_cookie);
		_cookie = 0;
	}
	if (_pCP)
	{
		_pCP->Release();
		_pCP = NULL;
	}
	if (_pIPAO)
	{
		_pIPAO->Release();
		_pIPAO = NULL;
	}
	if (_pIPO)
	{
		_pIPO->Release();
		_pIPO = NULL;
	}
	if (_pWB2)
	{
		_pWB2->Stop();
		_pWB2->Release();
		_pWB2 = NULL;
	}
	if (_pOle)
	{
		_pOle->Close(OLECLOSE_NOSAVE);
		_pOle->SetClientSite(NULL);
		_pOle->Release();
		_pOle = NULL;
	}
	_hwnd = NULL;
}

HRESULT CBrowserHost::InitEvents(void)
{
	IConnectionPointContainer *pCPC = NULL;
	HRESULT hr = S_FALSE;

	if (!_pWB2 || FAILED(_pWB2->QueryInterface(IID_IConnectionPointContainer, (void **)&pCPC)))
		return hr;

	if (SUCCEEDED(pCPC->FindConnectionPoint(DIID_DWebBrowserEvents2, &_pCP)))
		hr = _pCP->Advise((IDispatch *)(DWebBrowserEvents2 *)this, &_cookie);

	pCPC->Release();
	return hr;
}

void CBrowserHost::SetRect(const RECT *prc)
{
	_rc = *prc;
	if (_pIPO)
		_pIPO->SetObjectRects(&_rc, &_rc);
}

HRESULT CBrowserHost::Navigate(LPCWSTR url)
{
	if (!_pWB2)
		return E_FAIL;
	VARIANT v;
	VariantInit(&v);
	BSTR b = SysAllocString(url);
	HRESULT hr = _pWB2->Navigate(b, &v, &v, &v, &v);
	SysFreeString(b);
	return hr;
}

// ===================== navigation (the winsock http handler does the fetch) ===
// LoadUrl just navigates: ieproto.cpp's IInternetProtocol overrides the http namespace, so the
// control's normal bind is served over winsock and rendered by mshtml's native pipeline (no DOM
// injection, no file - the read-only DC FS and the broken inject APIs ruled those out).
HRESULT CBrowserHost::LoadUrl(LPCWSTR url)
{
	WCHAR nav[600];
	const WCHAR *rest;
	int i;
	if (!_pWB2 || !url)
		return E_FAIL;
	UiOnNavigate(url); // address bar shows the real http:// URL
	UiOnBusy(1);
	// Navigate via our private dcw:// scheme so EVERY bind phase reaches our winsock handler
	// (no WinInet fallback). Strip an http:// prefix; pass the remainder as dcw://<rest>.
	if (url[0] == L'd' && url[1] == L'c' && url[2] == L'w')
		return Navigate(url); // already dcw://
	rest = url;
	if (url[0] == L'h' && url[1] == L't' && url[2] == L't' && url[3] == L'p' && url[4] == L':' &&
	    url[5] == L'/' && url[6] == L'/')
		rest = url + 7;
	lstrcpyW(nav, L"dcw://");
	for (i = 0; rest[i] && i < 590; i++)
		nav[6 + i] = rest[i];
	nav[6 + i] = 0;
	return Navigate(nav);
}

void CBrowserHost::PumpPending(void)
{
} // no BeforeNavigate2 cancel needed anymore

HRESULT CBrowserHost::GoBack(void)
{
	return _pWB2 ? _pWB2->GoBack() : E_FAIL;
}
HRESULT CBrowserHost::GoForward(void)
{
	return _pWB2 ? _pWB2->GoForward() : E_FAIL;
}
HRESULT CBrowserHost::Refresh(void)
{
	return _pWB2 ? _pWB2->Refresh() : E_FAIL;
}
HRESULT CBrowserHost::Stop(void)
{
	return _pWB2 ? _pWB2->Stop() : E_FAIL;
}

HRESULT CBrowserHost::TranslateAccel(MSG *pMsg)
{
	return _pIPAO ? _pIPAO->TranslateAccelerator(pMsg) : S_FALSE;
}

HRESULT CBrowserHost::GetHtmlWindow(IHTMLWindow2 **ppWin)
{
	*ppWin = NULL;
	if (!_pWB2)
		return E_FAIL;
	IDispatch *pDisp = NULL;
	if (FAILED(_pWB2->get_Document(&pDisp)) || !pDisp)
		return E_FAIL;
	IHTMLDocument2 *pDoc = NULL;
	HRESULT hr = pDisp->QueryInterface(IID_IHTMLDocument2, (void **)&pDoc);
	pDisp->Release();
	if (FAILED(hr) || !pDoc)
		return E_FAIL;
	hr = pDoc->get_parentWindow(ppWin);
	pDoc->Release();
	return (*ppWin) ? S_OK : E_FAIL;
}

HRESULT CBrowserHost::ScrollBy(int dx, int dy)
{
	IHTMLWindow2 *pWin = NULL;
	if (FAILED(GetHtmlWindow(&pWin)) || !pWin)
		return E_FAIL;
	HRESULT hr = pWin->scrollBy((long)dx, (long)dy);
	pWin->Release();
	return hr;
}

// ===================== IUnknown ==========================================
STDMETHODIMP CBrowserHost::QueryInterface(REFIID riid, void **ppv)
{
	if (riid == IID_IUnknown)
		*ppv = (IUnknown *)(IOleClientSite *)this;
	else if (riid == IID_IOleClientSite)
		*ppv = (IOleClientSite *)this;
	else if (riid == IID_IOleWindow)
		*ppv = (IOleWindow *)(IOleInPlaceSite *)this;
	else if (riid == IID_IOleInPlaceSite)
		*ppv = (IOleInPlaceSite *)this;
	else if (riid == IID_IOleContainer)
		*ppv = (IOleContainer *)this;
	else if (riid == IID_IOleCommandTarget)
		*ppv = (IOleCommandTarget *)this;
	else if (riid == IID_IDispatch)
		*ppv = (IDispatch *)(DWebBrowserEvents2 *)this;
	else if (riid == DIID_DWebBrowserEvents2)
		*ppv = (DWebBrowserEvents2 *)this;
	else
	{
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	((IUnknown *)*ppv)->AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) CBrowserHost::AddRef(void)
{
	return ++_refs;
}
STDMETHODIMP_(ULONG) CBrowserHost::Release(void)
{
	if (--_refs == 0)
	{
		delete this;
		return 0;
	}
	return _refs;
}

// ===================== IOleClientSite ====================================
STDMETHODIMP CBrowserHost::SaveObject(void)
{
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::GetMoniker(DWORD, DWORD, IMoniker **p)
{
	if (p)
		*p = NULL;
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::GetContainer(IOleContainer **pp)
{
	return QueryInterface(IID_IOleContainer, (void **)pp);
}
STDMETHODIMP CBrowserHost::ShowObject(void)
{
	return S_OK;
}
STDMETHODIMP CBrowserHost::OnShowWindow(BOOL)
{
	return S_OK;
}
STDMETHODIMP CBrowserHost::RequestNewObjectLayout(void)
{
	return E_NOTIMPL;
}

// ===================== IOleWindow ========================================
STDMETHODIMP CBrowserHost::GetWindow(HWND *p)
{
	if (!p)
		return E_INVALIDARG;
	*p = _frame;
	return S_OK;
}
STDMETHODIMP CBrowserHost::ContextSensitiveHelp(BOOL)
{
	return E_NOTIMPL;
}

// ===================== IOleInPlaceSite ===================================
STDMETHODIMP CBrowserHost::CanInPlaceActivate(void)
{
	return S_OK;
}
STDMETHODIMP CBrowserHost::OnInPlaceActivate(void)
{
	return S_OK;
}
STDMETHODIMP CBrowserHost::OnUIActivate(void)
{
	return S_OK;
}
STDMETHODIMP CBrowserHost::GetWindowContext(IOleInPlaceFrame **ppFrame, IOleInPlaceUIWindow **ppDoc,
                                            LPRECT prcPos, LPRECT prcClip,
                                            LPOLEINPLACEFRAMEINFO pFI)
{
	if (ppFrame)
		*ppFrame = NULL; // no toolbar/menu negotiation - the host owns chrome
	if (ppDoc)
		*ppDoc = NULL;
	if (prcPos)
		*prcPos = _rc;
	if (prcClip)
		*prcClip = _rc;
	if (pFI)
	{
		pFI->cb = sizeof(OLEINPLACEFRAMEINFO);
		pFI->fMDIApp = FALSE;
		pFI->hwndFrame = _frame;
		pFI->haccel = NULL;
		pFI->cAccelEntries = 0;
	}
	return S_OK;
}
STDMETHODIMP CBrowserHost::Scroll(SIZE)
{
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::OnUIDeactivate(BOOL)
{
	return S_OK;
}
STDMETHODIMP CBrowserHost::OnInPlaceDeactivate(void)
{
	return S_OK;
}
STDMETHODIMP CBrowserHost::DiscardUndoState(void)
{
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::DeactivateAndUndo(void)
{
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::OnPosRectChange(LPCRECT)
{
	return S_OK;
}

// ===================== IOleContainer =====================================
STDMETHODIMP CBrowserHost::ParseDisplayName(IBindCtx *, LPOLESTR, ULONG *, IMoniker **)
{
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::EnumObjects(DWORD, IEnumUnknown **)
{
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::LockContainer(BOOL)
{
	return S_OK;
}

// ===================== IOleCommandTarget =================================
STDMETHODIMP CBrowserHost::QueryStatus(const GUID *pGroup, ULONG cCmds, OLECMD *rg,
                                       OLECMDTEXT *pText)
{
	if (pGroup)
		return OLECMDERR_E_UNKNOWNGROUP;
	if (pText && pText->cmdtextf != OLECMDTEXTF_NONE)
		pText->cwActual = 0;
	for (ULONG i = 0; i < cCmds; i++)
		rg[i].cmdf = 0;
	return S_OK;
}
STDMETHODIMP CBrowserHost::Exec(const GUID *pGroup, DWORD nCmdID, DWORD, VARIANTARG *pIn,
                                VARIANTARG *)
{
	if (pGroup)
		return OLECMDERR_E_UNKNOWNGROUP;
	if (nCmdID == OLECMDID_SETPROGRESSTEXT && pIn && pIn->vt == VT_BSTR)
	{
		UiOnStatus(pIn->bstrVal ? pIn->bstrVal : L"");
		return S_OK;
	}
	return OLECMDERR_E_NOTSUPPORTED;
}

// ===================== IDispatch (event sink) ============================
STDMETHODIMP CBrowserHost::GetTypeInfoCount(UINT *p)
{
	if (p)
		*p = 0;
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::GetTypeInfo(UINT, LCID, ITypeInfo **)
{
	return E_NOTIMPL;
}
STDMETHODIMP CBrowserHost::GetIDsOfNames(REFIID, OLECHAR **, UINT, LCID, DISPID *)
{
	return E_NOTIMPL;
}

STDMETHODIMP CBrowserHost::Invoke(DISPID dispid, REFIID, LCID, WORD, DISPPARAMS *pdp, VARIANT *,
                                  EXCEPINFO *, UINT *)
{
	if (!pdp)
		return S_OK;
	UINT n = pdp->cArgs;
	VARIANTARG *a = pdp->rgvarg; // reverse order: a[0] is the LAST param

	switch (dispid)
	{
		case DISPID_STATUSTEXTCHANGE:
			if (n >= 1 && a[0].vt == VT_BSTR && a[0].bstrVal)
				UiOnStatus(a[0].bstrVal);
			break;

		case DISPID_TITLECHANGE:
			if (n >= 1 && a[0].vt == VT_BSTR && a[0].bstrVal)
				UiOnTitle(a[0].bstrVal);
			break;

		case DISPID_BEFORENAVIGATE2:
		{
			// Let the navigation proceed - http binds go through our winsock IInternetProtocol and
			// render natively. Just track the address + busy state for the chrome (clicked links
			// fire this too, so they "just work").  a[n-2] = URL (byref variant BSTR).
			const WCHAR *url = NULL;
			OutputDebugStringW(L"IE evt: BeforeNavigate2\r\n");
			if (n >= 2 && a[n - 2].vt == (VT_BYREF | VT_VARIANT) && a[n - 2].pvarVal &&
			    a[n - 2].pvarVal->vt == VT_BSTR)
				url = a[n - 2].pvarVal->bstrVal;
			if (url && (url[0] == L'h' || url[0] == L'H'))
			{
				UiOnNavigate(url);
				UiOnBusy(1);
			}
			break;
		}

		case DISPID_NAVIGATECOMPLETE2:
		case DISPID_DOCUMENTCOMPLETE:
			// Don't surface the URL here: our internal navigations are about:blank /
			// file://\dcwie.htm, and LoadUrl already put the real address in the bar. Just clear
			// the busy flag.
			if (dispid == DISPID_DOCUMENTCOMPLETE)
			{
				OutputDebugStringW(L"IE evt: DocumentComplete\r\n");
				UiOnBusy(0);
			}
			break;

		case DISPID_DOWNLOADBEGIN:
			OutputDebugStringW(L"IE evt: DownloadBegin\r\n");
			UiOnBusy(1);
			break;
		case DISPID_DOWNLOADCOMPLETE:
			OutputDebugStringW(L"IE evt: DownloadComplete\r\n");
			UiOnBusy(0);
			break;

		// IE5+ navigation error (StatusCode in a byref-variant). Surfaces DNS/connect failures.
		case 271 /*DISPID_NAVIGATEERROR*/:
			OutputDebugStringW(L"IE evt: NavigateError\r\n");
			UiOnStatus(L"Navigation error");
			UiOnBusy(0);
			break;

#ifdef DISPID_SETSECURELOCKICON
		case DISPID_SETSECURELOCKICON:
			// arg is an int lock level (>0 => some https); treat nonzero as secure
			if (n >= 1)
			{
				int sec = 0;
				if (a[0].vt == VT_I4)
					sec = (a[0].lVal > 0);
				else if (a[0].vt == VT_BOOL)
					sec = (a[0].boolVal != 0);
				UiOnSecure(sec);
			}
			break;
#endif
	}
	return S_OK;
}
