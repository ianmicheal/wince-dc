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
	m_cRefs = 1;
	m_hwndFrame = hwndFrame;
	m_hwnd = NULL;
	m_rc = *prc;
	m_pWB2 = NULL;
	m_pOle = NULL;
	m_pIPO = NULL;
	m_pIPAO = NULL;
	m_pCP = NULL;
	m_dwCookie = 0;
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

	if (SUCCEEDED(pUnk->QueryInterface(IID_IOleObject, (void **)&m_pOle)))
	{
		DWORD dwMisc = 0;
		if (SUCCEEDED(m_pOle->GetMiscStatus(DVASPECT_CONTENT, &dwMisc)) &&
		    (dwMisc & OLEMISC_SETCLIENTSITEFIRST))
		{
			m_pOle->SetClientSite((IOleClientSite *)this);
		}

		// in-place UI activate inside the frame, sized to m_rc
		if (SUCCEEDED(m_pOle->DoVerb(OLEIVERB_UIACTIVATE, NULL, (IOleClientSite *)this, 0,
		                             m_hwndFrame, &m_rc)))
		{
			m_pOle->QueryInterface(IID_IOleInPlaceObject, (void **)&m_pIPO);
			if (SUCCEEDED(m_pOle->QueryInterface(IID_IWebBrowser2, (void **)&m_pWB2)))
				InitEvents();
		}
		pUnk->QueryInterface(IID_IOleInPlaceActiveObject, (void **)&m_pIPAO);
	}
	pUnk->Release();

	if (!m_pWB2)
	{
		OutputDebugStringW(L"IE: control activation FAILED\r\n");
		return FALSE;
	}

	// the control's window (parent for keyboard focus)
	{
		IOleWindow *pOW = NULL;
		if (SUCCEEDED(m_pWB2->QueryInterface(IID_IOleWindow, (void **)&pOW)))
		{
			pOW->GetWindow(&m_hwnd);
			pOW->Release();
		}
	}
	if (m_hwnd)
	{
		SetWindowLong(m_hwnd, GWL_EXSTYLE, GetWindowLong(m_hwnd, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
		SetFocus(m_hwnd);
	}
	OutputDebugStringW(L"IE: WebBrowser control up\r\n");
	return TRUE;
}

void CBrowserHost::Destroy(void)
{
	if (m_pCP && m_dwCookie)
	{
		m_pCP->Unadvise(m_dwCookie);
		m_dwCookie = 0;
	}
	if (m_pCP)
	{
		m_pCP->Release();
		m_pCP = NULL;
	}
	if (m_pIPAO)
	{
		m_pIPAO->Release();
		m_pIPAO = NULL;
	}
	if (m_pIPO)
	{
		m_pIPO->Release();
		m_pIPO = NULL;
	}
	if (m_pWB2)
	{
		m_pWB2->Stop();
		m_pWB2->Release();
		m_pWB2 = NULL;
	}
	if (m_pOle)
	{
		m_pOle->Close(OLECLOSE_NOSAVE);
		m_pOle->SetClientSite(NULL);
		m_pOle->Release();
		m_pOle = NULL;
	}
	m_hwnd = NULL;
}

HRESULT CBrowserHost::InitEvents(void)
{
	IConnectionPointContainer *pCPC = NULL;
	HRESULT hr = S_FALSE;

	if (!m_pWB2 || FAILED(m_pWB2->QueryInterface(IID_IConnectionPointContainer, (void **)&pCPC)))
		return hr;

	if (SUCCEEDED(pCPC->FindConnectionPoint(DIID_DWebBrowserEvents2, &m_pCP)))
		hr = m_pCP->Advise((IDispatch *)(DWebBrowserEvents2 *)this, &m_dwCookie);

	pCPC->Release();
	return hr;
}

void CBrowserHost::SetRect(const RECT *prc)
{
	m_rc = *prc;
	if (m_pIPO)
		m_pIPO->SetObjectRects(&m_rc, &m_rc);
}

HRESULT CBrowserHost::Navigate(LPCWSTR psz)
{
	if (!m_pWB2)
		return E_FAIL;
	VARIANT v;
	VariantInit(&v);
	BSTR b = SysAllocString(psz);
	HRESULT hr = m_pWB2->Navigate(b, &v, &v, &v, &v);
	SysFreeString(b);
	return hr;
}

// ===================== navigation (the winsock http handler does the fetch) ===
// LoadUrl just navigates: ieproto.cpp's IInternetProtocol overrides the http namespace, so the
// control's normal bind is served over winsock and rendered by mshtml's native pipeline (no DOM
// injection, no file - the read-only DC FS and the broken inject APIs ruled those out).
HRESULT CBrowserHost::LoadUrl(LPCWSTR psz)
{
	WCHAR szNav[600];
	const WCHAR *pszRest;
	int i;
	if (!m_pWB2 || !psz)
		return E_FAIL;
	UiOnNavigate(psz); // address bar shows the real http:// URL
	UiOnBusy(1);
	// Navigate via our private dcw:// scheme so EVERY bind phase reaches our winsock handler
	// (no WinInet fallback). Strip an http:// prefix; pass the remainder as dcw://<pszRest>.
	if (psz[0] == L'd' && psz[1] == L'c' && psz[2] == L'w')
		return Navigate(psz); // already dcw://
	pszRest = psz;
	if (psz[0] == L'h' && psz[1] == L't' && psz[2] == L't' && psz[3] == L'p' && psz[4] == L':' &&
	    psz[5] == L'/' && psz[6] == L'/')
		pszRest = psz + 7;
	lstrcpyW(szNav, L"dcw://");
	for (i = 0; pszRest[i] && i < 590; i++)
		szNav[6 + i] = pszRest[i];
	szNav[6 + i] = 0;
	return Navigate(szNav);
}

void CBrowserHost::PumpPending(void)
{
} // no BeforeNavigate2 cancel needed anymore

HRESULT CBrowserHost::GoBack(void)
{
	return m_pWB2 ? m_pWB2->GoBack() : E_FAIL;
}
HRESULT CBrowserHost::GoForward(void)
{
	return m_pWB2 ? m_pWB2->GoForward() : E_FAIL;
}
HRESULT CBrowserHost::Refresh(void)
{
	return m_pWB2 ? m_pWB2->Refresh() : E_FAIL;
}
HRESULT CBrowserHost::Stop(void)
{
	return m_pWB2 ? m_pWB2->Stop() : E_FAIL;
}

HRESULT CBrowserHost::TranslateAccel(MSG *pMsg)
{
	return m_pIPAO ? m_pIPAO->TranslateAccelerator(pMsg) : S_FALSE;
}

HRESULT CBrowserHost::GetHtmlWindow(IHTMLWindow2 **ppWin)
{
	*ppWin = NULL;
	if (!m_pWB2)
		return E_FAIL;
	IDispatch *pDisp = NULL;
	if (FAILED(m_pWB2->get_Document(&pDisp)) || !pDisp)
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
	return ++m_cRefs;
}
STDMETHODIMP_(ULONG) CBrowserHost::Release(void)
{
	if (--m_cRefs == 0)
	{
		delete this;
		return 0;
	}
	return m_cRefs;
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
	*p = m_hwndFrame;
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
		*prcPos = m_rc;
	if (prcClip)
		*prcClip = m_rc;
	if (pFI)
	{
		pFI->cb = sizeof(OLEINPLACEFRAMEINFO);
		pFI->fMDIApp = FALSE;
		pFI->hwndFrame = m_hwndFrame;
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
	UINT cArgs = pdp->cArgs;
	VARIANTARG *aArg = pdp->rgvarg; // reverse order: aArg[0] is the LAST param

	switch (dispid)
	{
		case DISPID_STATUSTEXTCHANGE:
			if (cArgs >= 1 && aArg[0].vt == VT_BSTR && aArg[0].bstrVal)
				UiOnStatus(aArg[0].bstrVal);
			break;

		case DISPID_TITLECHANGE:
			if (cArgs >= 1 && aArg[0].vt == VT_BSTR && aArg[0].bstrVal)
				UiOnTitle(aArg[0].bstrVal);
			break;

		case DISPID_BEFORENAVIGATE2:
		{
			// Let the navigation proceed - http binds go through our winsock IInternetProtocol and
			// render natively. Just track the address + busy state for the chrome (clicked links
			// fire this too, so they "just work").  aArg[cArgs-2] = URL (byref variant BSTR).
			const WCHAR *pszUrl = NULL;
			OutputDebugStringW(L"IE evt: BeforeNavigate2\r\n");
			if (cArgs >= 2 && aArg[cArgs - 2].vt == (VT_BYREF | VT_VARIANT) &&
			    aArg[cArgs - 2].pvarVal && aArg[cArgs - 2].pvarVal->vt == VT_BSTR)
				pszUrl = aArg[cArgs - 2].pvarVal->bstrVal;
			if (pszUrl && (pszUrl[0] == L'h' || pszUrl[0] == L'H'))
			{
				UiOnNavigate(pszUrl);
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
			if (cArgs >= 1)
			{
				int bSecure = 0;
				if (aArg[0].vt == VT_I4)
					bSecure = (aArg[0].lVal > 0);
				else if (aArg[0].vt == VT_BOOL)
					bSecure = (aArg[0].boolVal != 0);
				UiOnSecure(bSecure);
			}
			break;
#endif
	}
	return S_OK;
}
