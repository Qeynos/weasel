#include "stdafx.h"
#include <WeaselUI.h>
#include "WeaselPanel.h"

using namespace weasel;

class weasel::UIImpl {
 public:
  WeaselPanel panel;

  UIImpl(weasel::UI& ui) : panel(ui), shown(false) {}
  ~UIImpl() {}
  void Refresh() {
    if (!panel.IsWindow())
      return;
    if (timer) {
      Hide();
      KillTimer(panel.m_hWnd, AUTOHIDE_TIMER);
      timer = 0;
    }
    panel.Refresh();
  }
  void Show();
  void Hide();
  void ShowWithTimeout(size_t millisec);
  bool IsShown() const { return shown; }

  static VOID CALLBACK OnTimer(_In_ HWND hwnd,
                               _In_ UINT uMsg,
                               _In_ UINT_PTR idEvent,
                               _In_ DWORD dwTime);
  static const int AUTOHIDE_TIMER = 20121220;
  static const int SHOW_TIMER = 20121221;
  // 新建窗口的首次顯示延遲：默認 25ms，之後按本進程實際觀測到的“位置穩定
  // 時間”自適應調整(見 OnPositionApplied)，範圍 [15, 60]ms。
  static const DWORD DEFAULT_SHOW_DELAY_MS = 25;
  DWORD showDelayMs = DEFAULT_SHOW_DELAY_MS;
  // 觀測:Show 被請求後，位置從“舊採樣”變成“正確位置”花了多久(EMA)
  DWORD settleEma = 0;
  DWORD settleStart = 0;
  RECT settlePos = {0, 0, 0, 0};
  bool settleWatch = false;
  static UINT_PTR timer;
  static UIImpl* show_self;
  bool shown;
  // 延遲顯示：組字瞬間 app 的前幾個位置採樣可能是上一次的光標，立即顯示就會
  // 在舊位置閃一下。排程 45ms 後再顯示，那時位置通常已穩定。
  bool showPending = false;
  // 窗口是不是本輪組字剛建出來的：新建窗口的第一次顯示要等一下位置採樣穩定，
  // 否則會先用上一次組字的位置閃一下。已可見後的顯示則立即執行。
  bool freshWindow = true;
  void OnPositionApplied();
  void ResetShowState() {
    showPending = false;
    freshWindow = true;
    settleWatch = false;
    if (show_self == this)
      show_self = nullptr;
    if (panel.IsWindow())
      KillTimer(panel.m_hWnd, SHOW_TIMER);
  }
};

UINT_PTR UIImpl::timer = 0;
UIImpl* UIImpl::show_self = nullptr;

void UIImpl::Show() {
  if (!panel.IsWindow())
    return;

  if (timer) {
    KillTimer(panel.m_hWnd, AUTOHIDE_TIMER);
    timer = 0;
  }

  if (panel.IsWindowVisible()) {
    shown = true;
    return;
  }

  // 剛建出來的新窗口：先等一下，讓這一輪的位置採樣穩定（前幾個採樣往往
  // 還是上一次組字的光標）。已經顯示過的窗口直接顯示，不引入額外延遲。
  if (!freshWindow) {
    panel.ShowWindow(SW_SHOWNA);
    shown = true;
    return;
  }

  if (showPending && show_self == this)
    return;  // 已經排程，不要重複延後

  showPending = true;
  show_self = this;
  // 記錄“等待起點”，供 OnPositionApplied 觀測本程序的實際穩定時間
  settleWatch = true;
  settleStart = ::GetTickCount();
  settlePos = RECT{0, 0, 0, 0};
  panel.GetWindowRect(&settlePos);
  SetTimer(panel.m_hWnd, SHOW_TIMER, showDelayMs, &UIImpl::OnTimer);
}

// 位置被真正應用時回調(UI::UpdateInputPosition 之後)。如果在“等待顯示”期間
// 位置發生了變化，說明這幾個採樣還是上一次組字的光標；觀測從 Show 到穩定的
// 間隔，用 EMA 估計下一次該等多久，讓不同設備/不同應用各自收斂。
void UIImpl::OnPositionApplied() {
  if (!settleWatch)
    return;
  RECT wr = {0, 0, 0, 0};
  if (!panel.IsWindow() || !panel.GetWindowRect(&wr))
    return;
  if (wr.left == settlePos.left && wr.top == settlePos.top)
    return;  // 位置沒變，這一幀不是抖動
  const DWORD sample = ::GetTickCount() - settleStart;
  settleWatch = false;
  if (sample > 500)
    return;  // 是使用者真的換了位置，不是採樣抖動
  settleEma = (settleEma == 0) ? sample : (settleEma * 3 + sample) / 4;
  DWORD want = settleEma * 3 / 2 + 5;  // 1.5 倍 + 5ms 餘量
  if (want < 15)
    want = 15;
  if (want > 60)
    want = 60;
  showDelayMs = want;
}

void UIImpl::Hide() {
  if (!panel.IsWindow())
    return;
  panel.ShowWindow(SW_HIDE);
  shown = false;
  showPending = false;
  show_self = nullptr;
  if (panel.IsWindow())
    KillTimer(panel.m_hWnd, SHOW_TIMER);
  if (timer) {
    KillTimer(panel.m_hWnd, AUTOHIDE_TIMER);
    timer = 0;
  }
}

void UIImpl::ShowWithTimeout(size_t millisec) {
  if (!panel.IsWindow())
    return;
  DLOG(INFO) << "ShowWithTimeout: " << millisec;
  panel.ShowWindow(SW_SHOWNA);
  shown = true;
  SetTimer(panel.m_hWnd, AUTOHIDE_TIMER, static_cast<UINT>(millisec),
           &UIImpl::OnTimer);
  timer = UINT_PTR(this);
}
VOID CALLBACK UIImpl::OnTimer(_In_ HWND hwnd,
                              _In_ UINT uMsg,
                              _In_ UINT_PTR idEvent,
                              _In_ DWORD dwTime) {
  UITryLock guard;
  if (!guard.owns())  // 管道線程持鎖時不要阻塞窗口線程
    return;
  DLOG(INFO) << "OnTimer:";
  if (idEvent == UIImpl::SHOW_TIMER) {
    KillTimer(hwnd, UIImpl::SHOW_TIMER);
    UIImpl* self = UIImpl::show_self;
    UIImpl::show_self = nullptr;
    if (self && self->showPending) {
      self->showPending = false;
      if (self->panel.IsWindow()) {
        self->panel.ShowWindow(SW_SHOWNA);
        self->shown = true;
      }
    }
    return;
  }
  KillTimer(hwnd, idEvent);
  UIImpl* self = (UIImpl*)timer;
  timer = 0;
  if (self) {
    self->Hide();
    self->shown = false;
  }
}

bool UI::Create(HWND parent) {
  UILock guard;
  if (pimpl_) {
    pimpl_->ResetShowState();
    pimpl_->panel.Create(
        parent, 0, 0, WS_POPUP,
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        0U, 0);
    return true;
  }

  pimpl_ = new UIImpl(*this);
  if (!pimpl_)
    return false;

  pimpl_->panel.Create(
      parent, 0, 0, WS_POPUP,
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      0U, 0);
  return true;
}

void UI::Destroy(bool full) {
  UILock guard;
  if (pimpl_) {
    pimpl_->ResetShowState();
    // destroy panel
    if (pimpl_->panel.IsWindow()) {
      pimpl_->panel.DestroyWindow();
    }
    if (full) {
      delete pimpl_;
      pimpl_ = 0;
      pDWR.reset();
    }
  }
}

bool UI::GetIsReposition() {
  UILock guard;
  if (pimpl_)
    return pimpl_->panel.GetIsReposition();
  else
    return false;
}

void UI::Show() {
  UILock guard;
  if (pimpl_) {
    pimpl_->Show();
  }
}

void UI::Hide() {
  UILock guard;
  if (pimpl_) {
    pimpl_->Hide();
  }
}

void UI::ShowWithTimeout(size_t millisec) {
  UILock guard;
  if (pimpl_) {
    pimpl_->ShowWithTimeout(millisec);
  }
}

bool UI::IsCountingDown() const {
  UILock guard;
  return pimpl_ && pimpl_->timer != 0;
}

bool UI::IsShown() const {
  UILock guard;
  return pimpl_ && pimpl_->IsShown();
}

void UI::Refresh() {
  UILock guard;
  if (pimpl_) {
    pimpl_->Refresh();
  }
}

void UI::UpdateInputPosition(RECT const& rc) {
  UILock guard;
  if (pimpl_ && pimpl_->panel.IsWindow()) {
    pimpl_->panel.MoveTo(rc);
    pimpl_->OnPositionApplied();
  }
}

void UI::Update(const Context& ctx, const Status& status) {
  UILock guard;
  if (ctx_ == ctx && status_ == status)
    return;
  ctx_ = ctx;
  status_ = status;
  if (style_.candidate_abbreviate_length > 0) {
    for (auto& c : ctx_.cinfo.candies) {
      if (c.str.length() > (size_t)style_.candidate_abbreviate_length) {
        c.str =
            c.str.substr(0, (size_t)style_.candidate_abbreviate_length - 1) +
            L"..." + c.str.substr(c.str.length() - 1);
      }
    }
  }
  Refresh();
}
