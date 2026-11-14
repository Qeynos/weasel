#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"

STDAPI WeaselTSF::OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) {
  return S_OK;
}

STDAPI WeaselTSF::OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) {
  return S_OK;
}

STDAPI WeaselTSF::OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                             ITfDocumentMgr* pDocMgrPrevFocus) {
  _InitTextEditSink(pDocMgrFocus);

    com_ptr<ITfDocumentMgr> pCandidateListDocumentMgr;
  com_ptr<ITfContext> pTfContext = _GetUIContextDocument();
  if ((nullptr != pTfContext) &&
      SUCCEEDED(pTfContext->GetDocumentMgr(&pCandidateListDocumentMgr))) {
    // 焦點切換後舊位置不再可信：任何一次顯示都必須等新的採樣
    _cand->InvalidatePosition();
    if (pCandidateListDocumentMgr != pDocMgrFocus) {
      _HideUI();
    }
    // 注意：這裡原本還有一個 else { _ShowUI(); }，會在「同一個 document 內
    // 換焦點 / 換輸入框」時把候選窗直接顯示出來，而此時既沒有新的
    // composition 也沒有新的位置，於是它出現在上一次輸入的位置，看起來就是
    // 「在上次的位置閃一下」。顯示的時機統一交給 UpdateUI（它會先定位再顯示）。
  }

  return S_OK;
}

STDAPI WeaselTSF::OnPushContext(ITfContext* pContext) {
  return S_OK;
}

STDAPI WeaselTSF::OnPopContext(ITfContext* pContext) {
  return S_OK;
}

BOOL WeaselTSF::_InitThreadMgrEventSink() {
  ITfSource* pSource;
  if (_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource) != S_OK)
    return FALSE;
  if (pSource->AdviseSink(IID_ITfThreadMgrEventSink,
                          (ITfThreadMgrEventSink*)this,
                          &_dwThreadMgrEventSinkCookie) != S_OK) {
    _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    pSource->Release();
    return FALSE;
  }
  pSource->Release();
  return TRUE;
}

void WeaselTSF::_UninitThreadMgrEventSink() {
  ITfSource* pSource;
  if (_dwThreadMgrEventSinkCookie == TF_INVALID_COOKIE)
    return;
  if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource))) {
    pSource->UnadviseSink(_dwThreadMgrEventSinkCookie);
    pSource->Release();
  }
  _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
}
