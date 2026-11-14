#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled,
                               BOOL inlinePreeditEnabled)
      : CEditSession(pTextService, pContext),
        _inlinePreeditEnabled(inlinePreeditEnabled) {
    _fCUASWorkaroundEnabled = fCUASWorkaroundEnabled;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
  BOOL _inlinePreeditEnabled;
};

STDAPI CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return hr;
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return hr;

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return hr;
  if ((pContextComposition->StartComposition(
           ec, pRangeComposition, _pTextService, &pComposition) == S_OK) &&
      (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    /* WORKAROUND:
     *   CUAS does not provide a correct GetTextExt() position unless the
     * composition is filled with characters. So we insert a zero width space
     * here. The workaround is only needed when inline preedit is not enabled.
     *   See https://github.com/rime/weasel/pull/883#issuecomment-1567625762
     */
    if (!_inlinePreeditEnabled) {
      pRangeComposition->SetText(ec, TF_ST_CORRECTION, L" ", 1);
    }

    /* set selection */
    TF_SELECTION tfSelection;
    if (_inlinePreeditEnabled)
      pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    else
      pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    tfSelection.range = pRangeComposition;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);
  }

  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
    // 清掉上一輪的去抖狀態：否則新組字的第一個採樣（往往還是上一輪的位置）
  // 會跟殘留的 _pendingPos 相同而被當成「已穩定」，面板就先在舊位置顯示。
  _pendingSince = 0;
  _pendingPos = RECT{0, 0, 0, 0};
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(new CStartCompositionEditSession(
      this, pContext, fCUASWorkaroundEnabled, _cand->style().inline_preedit));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE)
      : CEditSession(pTextService, pContext), _clear(clear) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
};

STDAPI CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  ITfRange* pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK)
    pCompositionRange->SetText(ec, 0, L"", 0);

  _pComposition->EndComposition(ec);
  // 只有「被結束的這個」仍是目前的 composition 時才清空。
  // 否則（使用者在結束的 session 執行前就已經開始了新一輪組字）會把新的
  // _pComposition 清掉，接下來的取位置就退回游標，面板會跳到另一個位置。
  com_ptr<ITfComposition> pCurrent = _pTextService->_GetComposition();
  if (pCurrent == nullptr || pCurrent.IsEqualObject(_pComposition))
    _pTextService->_FinalizeComposition();
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext, BOOL clear) {
    _pendingSince = 0;
  _pendingPos = RECT{0, 0, 0, 0};
  CEndCompositionEditSession* pEditSession;
  HRESULT hr;

  _cand->EndUI();
  if ((pEditSession = new CEndCompositionEditSession(
           this, pContext, _pComposition, clear)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }
}

/* Get Text Extent */

// 取得 composition 起點在螢幕上的位置。
// TSF 規範要求 ITfContextView::GetTextExt 回傳「螢幕座標」，但少數程式
// (例如 Chromium 系) 在部分情況下回傳的是 client 座標。與其用
// GetForegroundWindow/GetCaretPos 去猜（那會在相鄰兩幀之間一真一假，
// 正是候選窗跳動的來源），這裡直接和視窗自己的矩形比對：
//   1) rc 落在視窗螢幕矩形內  -> 就是螢幕座標；
//   2) rc 落在 client 矩形內   -> 加上視窗原點換算成螢幕座標；
//   3) 兩者都不是              -> 視為無效採樣，丟棄。
static bool GetCompositionScreenRect(ITfContextView* pContextView,
                                     TfEditCookie ec,
                                     ITfRange* pRange,
                                     RECT& rc,
                                     bool allow_client_coords) {
  if (!pContextView || !pRange)
    return false;

  RECT r = {0, 0, 0, 0};
  BOOL fClipped = FALSE;
  if (pContextView->GetTextExt(ec, pRange, &r, &fClipped) != S_OK)
    return false;
  // 只擋「明顯壞掉」的資料：反向或全零。
  // 零寬/零高是合法的（- range 被 collapse 成插入點時，多數實作就是回一個
  // 零寬矩形；某些實作連高度都是 0），之前把它當壞資料丟掉，結果在 Chrome
  // 這類一直回零高矩形的程式裡所有採樣都被丟棄，候選窗永遠停在初始的左上角。
  if (r.right < r.left || r.bottom < r.top)
    return false;
  if (r.left == 0 && r.top == 0 && r.right == 0 && r.bottom == 0)
    return false;

  // 預設：TSF 規範就是螢幕座標，先照單全收
  rc = r;

  HWND hwnd = NULL;
  if (SUCCEEDED(pContextView->GetWnd(&hwnd)) && hwnd != NULL) {
    RECT wr = {0, 0, 0, 0};
    RECT cr = {0, 0, 0, 0};
    if (::GetWindowRect(hwnd, &wr) && ::GetClientRect(hwnd, &cr)) {
      POINT origin = {0, 0};
      if (::ClientToScreen(hwnd, &origin)) {
        RECT sc = cr;  // client 矩形換算到螢幕座標
        ::OffsetRect(&sc, origin.x, origin.y);
        const auto pt_inside = [](LONG x, LONG y, const RECT& b) {
          return x >= b.left && x <= b.right && y >= b.top && y <= b.bottom;
        };
        // 用插入點（左上角）判斷屬於哪個座標系：整個矩形的包含判斷太嚴，
        // 游標貼著視窗邊緣時會被誤判成「不在視窗內」而丟棄。
        if (!pt_inside(r.left, r.top, sc) && allow_client_coords &&
            pt_inside(r.left, r.top, cr)) {
          // client 座標 -> 加上視窗原點
          rc.left = r.left + origin.x;
          rc.right = r.right + origin.x;
          rc.top = r.top + origin.y;
          rc.bottom = r.bottom + origin.y;
        }
        // 兩個座標系都不在（或取不到視窗）：維持「當作螢幕座標」，
        // 寧可位置略有偏差，也不要整場丟棄導致面板卡在左上角。
      }
    }
  }

  // 必須落在某個顯示器上，否則視為垃圾採樣
  if (::MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) == NULL)
    return false;

  return true;
}

// 同時取「composition 起點」和「游標」兩個位置並互相校驗：
//  - Chromium 系：兩者其實是同一個 caret（它對 collapse 的 range 回 caret），
//    取 composition 起點即可，配合去抖就穩定；
//  - Scintilla/Notepad3：composition 的 range 有時落在文件開頭（實測相差 700+px），
//    此時以游標為準，否認就會把候選窗放到文件頂端。
// 判定很簡單：兩者相差超過「一行高」或「三字寬」就認為 range 不可靠。
static bool GetCompositionOrCaretRect(ITfContext* pContext,
                                      ITfContextView* pContextView,
                                      TfEditCookie ec,
                                      ITfComposition* pComposition,
                                      RECT& rc,
                                      bool allow_client_coords,
                                      bool* from_selection = nullptr) {
  com_ptr<ITfRange> compositionRange;
  RECT rc_comp = {0, 0, 0, 0};
  bool ok_comp = false;
  if (pComposition != nullptr &&
      pComposition->GetRange(&compositionRange) == S_OK) {
    compositionRange->Collapse(ec, TF_ANCHOR_START);
    ok_comp = GetCompositionScreenRect(pContextView, ec, compositionRange,
                                       rc_comp, allow_client_coords);
  }

  com_ptr<ITfRange> selectionRange;
  RECT rc_sel = {0, 0, 0, 0};
  bool ok_sel = false;
  TF_SELECTION selection = {0};
  ULONG nSelection = 0;
  if (pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                             &nSelection) == S_OK &&
      nSelection == 1 && selection.range != NULL) {
    selectionRange.Attach(selection.range);
    ok_sel = GetCompositionScreenRect(pContextView, ec, selectionRange, rc_sel,
                                      allow_client_coords);
  }

  if (ok_comp && ok_sel) {
    const LONG dx = labs(rc_comp.left - rc_sel.left);
    const LONG dy = labs(rc_comp.bottom - rc_sel.bottom);
    const bool range_unreliable = (dx > 120 || dy > 40);
    rc = range_unreliable ? rc_sel : rc_comp;
    if (from_selection)
      *from_selection = range_unreliable;
    return true;
  }
  if (ok_comp) {
    rc = rc_comp;
    if (from_selection)
      *from_selection = false;
    return true;
  }
  if (ok_sel) {
    rc = rc_sel;
    if (from_selection)
      *from_selection = true;
    return true;
  }
  return false;
}

class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition,
                            ULONG generation)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
    _generation = generation;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
  ULONG _generation;
};

STDAPI CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  if (!_pContext || !_pContextView)
    return E_FAIL;

  // 用 *執行時* 的 composition（不是捕獲的舊物件），並同時取 composition 起點
  // 和游標，兩者嚴重不一致時以游標為準（Scintilla 的 range 會落在文件開頭）。
  com_ptr<ITfComposition> pCurrentComposition = _pTextService->_GetComposition();

  // 這個採樣若已被更新的請求取代，直接丟棄：異步 edit session 可能亂序返回，
  // 舊結果套用下去就是候選窗來回跳。
  if (_generation != 0 &&
      _generation != _pTextService->_GetPositionGeneration())
    return S_OK;

  RECT rc = {0, 0, 0, 0};
  bool from_sel = false;
  bool ok = GetCompositionOrCaretRect(_pContext, _pContextView, ec,
                                      pCurrentComposition, rc,
                                      _enhancedPosition, &from_sel);
  ULONG cur = _pTextService->_GetPositionGeneration();
    if (ok)
    _pTextService->_SetCompositionPosition(rc);
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(new CGetTextExtentEditSession(
      this, pContext, pContextView, _pComposition,
      _cand->style().enhanced_position, ++_positionGeneration));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  return SUCCEEDED(hr);
}

// 同步取一次位置。由 DoEditSession 在 Show 之前呼叫，這樣候選窗第一次出現
// 就在游標處，而不是先出現在上一次的位置（可能是另一個視窗）再跳過來。
BOOL WeaselTSF::_UpdateCompositionPosition(ITfContext* pContext,
                                            TfEditCookie ec) {
  if (!pContext)
    return FALSE;

  // 作廢所有在途的異步採樣，避免舊結果稍後覆蓋這次的結果
  ++_positionGeneration;

  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK || !pContextView)
    return FALSE;

  // 與異步路徑同一規則：同時取 composition 起點和游標，不一致時用游標
  RECT rc = {0, 0, 0, 0};
  bool from_sel = false;
  bool ok = GetCompositionOrCaretRect(pContext, pContextView, ec, _pComposition,
                                      rc, _cand->style().enhanced_position,
                                      &from_sel);
    if (!ok)
    return FALSE;

  _SetCompositionPosition(rc);
  return TRUE;
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
    /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  // 去抖（按時間，不是按次數）：
  //  - Chromium 系對 collapse 過的 range 呼叫 GetTextExt 實際回的是當下 caret，
  //    而 caret 被我們自己的 edit session 在組字起點/preedit 末尾之間搬動，
  //    同一段時間內會收到相隔約一個字寬的兩個位置；
  //  - Notepad3 換位置後的前兩個採樣（sync+async，相隔 1ms）都還是上一次的光標，
  //    真正的游標位置要到下一輪採樣才出現。
  // 所以「連續兩次相同」不夠（sync+async 天生就是兩次）：要求同一位置至少穩定
  // kStableMs 才真正移動面板。若一直不穩定，超過 kMaxWaitMs 就採用最新值，
  // 避免延遲無限累積。定位未完成前面板不顯示（見 CCandidateList::UpdateUI）。
  const DWORD now = ::GetTickCount();
  const bool same_pending =
      (rc.left == _pendingPos.left && rc.top == _pendingPos.top &&
       rc.right == _pendingPos.right && rc.bottom == _pendingPos.bottom);
  if (!same_pending) {
    if (_pendingSince == 0)
      _pendingSince = now;
    if (now - _pendingSince < 150) {
      _pendingPos = rc;
            return;
    }
      }
  _pendingSince = 0;
  _pendingPos = rc;

  RECT _rc;
  _rc.left = _rc.right = rc.left;
  _rc.top = _rc.bottom = rc.bottom;
  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc);
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDAPI CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
  _UpdateCompositionWindow(pContext);
}

/* Composition State */
STDAPI WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                          ITfComposition* pComposition) {
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear) {
  m_client.ClearComposition();
  if (_IsComposing()) {
    _EndComposition(_pEditSessionContext, clear);
  }
  _committed = TRUE;
  _cand->Destroy();
}

void WeaselTSF::_FinalizeComposition() {
  _pComposition = nullptr;
}

void WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition) {
  _pComposition = pComposition;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}
