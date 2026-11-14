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
  static UINT_PTR timer;
  static UIImpl* show_self;
  bool shown;
  // 延遲顯示：組字瞬間 app 的前幾個位置採樣可能是上一次的光標，立即顯示就會
  // 在舊位置閃一下。排程 45ms 後再顯示，那時位置通常已穩定。
  bool showPending = false;
  // 窗口是不是本輪組字剛建出來的：新建窗口的第一次顯示要等一下位置採樣穩定，
  // 否則會先用上一次組字的位置閃一下。已可見後的顯示則立即執行。
  bool freshWindow = true;
  void ResetShowState() {
    showPending = false;
    freshWindow = true;
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

  // 剛建出來的新窗口：先等 45ms，讓這一輪的位置採樣穩定（前幾個採樣往往
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
  SetTimer(panel.m_hWnd, SHOW_TIMER, 45, &UIImpl::OnTimer);
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
  bool has_window = pimpl_ && pimpl_->panel.IsWindow();
    if (has_window) {
    pimpl_->panel.MoveTo(rc);
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
