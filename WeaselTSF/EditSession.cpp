#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"

STDAPI WeaselTSF::DoEditSession(TfEditCookie ec) {
  // 最後一道防線：這是 TSF 回調，任何異常穿出去都會被 Windows 以
  // 0xc000041d(fatal user callback exception) 終止整個宿主進程(explorer、
  // 瀏覽器、編輯器……)。IPC 層已各自兜底，這裡再包一層防止遺漏的來源
  // (例如內存不足時 std:: 容器拋出的異常)。
  try {
    // get commit string from server
    std::wstring commit;
    weasel::Config config;
    auto context = std::make_shared<weasel::Context>();
    weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                  &_cand->style());

    bool ok = m_client.GetResponseData(std::ref(parser));

    _UpdateLanguageBar(_status);

    if (ok) {
      if (!commit.empty()) {
        // For auto-selecting, commit and preedit can both exist.
        // Commit and close the original composition first.
        if (!_IsComposing()) {
          _StartComposition(_pEditSessionContext,
                            _fCUASWorkaroundEnabled && !config.inline_preedit);
        }
        _InsertText(_pEditSessionContext, commit);
        _EndComposition(_pEditSessionContext, false);
        _committed = TRUE;
      } else {
        _committed = FALSE;
      }
      if (_status.composing && !_IsComposing()) {
        _StartComposition(_pEditSessionContext,
                          _fCUASWorkaroundEnabled && !config.inline_preedit);
      } else if (!_status.composing && _IsComposing()) {
        _EndComposition(_pEditSessionContext, true);
      }
      if (_IsComposing() && config.inline_preedit) {
        _ShowInlinePreedit(_pEditSessionContext, context);
      }
      // 先在這個 edit session 裡同步算好位置，再交給 _UpdateUI 去 Show：
      // 否則候選窗會先用上一次的位置顯示，等異步的 GetTextExt 回來才跳過去。
      _UpdateCompositionPosition(_pEditSessionContext, ec);
      _UpdateCompositionWindow(_pEditSessionContext);
    }

    _UpdateUI(*context, _status);
  } catch (...) {
    return S_OK;
  }

  return TRUE;
}
