#include "stdafx.h"
#include "WeaselTrayIcon.h"
#include <atlstr.h>

// nasty
#include <resource.h>

static UINT mode_icon[] = {IDI_ZH, IDI_ZH, IDI_EN, IDI_RELOAD};
static const WCHAR* mode_label[] = {NULL, /*L"中文"*/ NULL, /*L"西文"*/ NULL,
                                    L"Under maintenance"};

WeaselTrayIcon::WeaselTrayIcon(weasel::UI& ui)
    : m_style(ui.style()),
      m_status(ui.status()),
      m_mode(INITIAL),
      m_schema_zhung_icon(),
      m_schema_ascii_icon(),
      m_disabled(false) {}

void WeaselTrayIcon::CustomizeMenu(HMENU hMenu) {}

BOOL WeaselTrayIcon::Create(HWND hTargetWnd) {
  HMODULE hModule = GetModuleHandle(NULL);
  CIcon icon;
  icon.LoadIconW(IDI_ZH);
  BOOL bRet =
      CSystemTray::Create(hModule, NULL, WM_WEASEL_TRAY_NOTIFY,
                          get_weasel_ime_name().c_str(), icon, IDR_MENU_POPUP);
  if (hTargetWnd) {
    SetTargetWnd(hTargetWnd);
  }
  if (!m_style.display_tray_icon) {
    RemoveIcon();
  } else {
    AddIcon();
  }
  return bRet;
}

void WeaselTrayIcon::Refresh() {
  // m_style / m_status 是 weasel::UI 里的对象引用，会被 IPC 线程改写，
  // 因此先在锁内取快照；而 Shell_NotifyIcon 会同步通知 explorer 的托盘窗口，
  // 绝不能在持锁时调用（explorer 自己也可能正在等我们的管道）。
  bool display_tray_icon = false;
  bool disabled = false;
  bool ascii_mode = false;
  std::wstring zhung_icon;
  std::wstring ascii_icon;
  {
    weasel::UITryLock guard;
    if (!guard.owns())  // 管道線程正在更新界面，跳過這一輪
      return;
    display_tray_icon = m_style.display_tray_icon;
    disabled = m_status.disabled;
    ascii_mode = m_status.ascii_mode;
    zhung_icon = m_style.current_zhung_icon;
    ascii_icon = m_style.current_ascii_icon;
  }

  if (!display_tray_icon && !disabled)  // display notification when deploying
  {
    if (m_mode != INITIAL) {
      RemoveIcon();
      m_mode = INITIAL;
    }
    m_disabled = false;
    return;
  }
  WeaselTrayMode mode =
      disabled ? DISABLED : ascii_mode ? ASCII : ZHUNG;
  /* change icon, when
          1,mode changed
          2,icon changed
          3,both m_schema_zhung_icon and zhung_icon empty(for initialize)
          4,both m_schema_ascii_icon and ascii_icon empty(for initialize)
  */
  if (mode != m_mode || m_schema_zhung_icon != zhung_icon ||
      (m_schema_zhung_icon.empty() && zhung_icon.empty()) ||
      m_schema_ascii_icon != ascii_icon ||
      (m_schema_ascii_icon.empty() && ascii_icon.empty())) {
    ShowIcon();
    m_mode = mode;
    m_schema_zhung_icon = zhung_icon;
    m_schema_ascii_icon = ascii_icon;
    if (mode == ASCII) {
      if (m_schema_ascii_icon.empty())
        SetIcon(mode_icon[mode]);
      else
        SetIcon(m_schema_ascii_icon.c_str());
    } else if (mode == ZHUNG) {
      if (m_schema_zhung_icon.empty())
        SetIcon(mode_icon[mode]);
      else
        SetIcon(m_schema_zhung_icon.c_str());
    } else
      SetIcon(mode_icon[mode]);

    if (mode_label[mode] && m_disabled == false) {
      CString info;
      info.LoadStringW(IDS_STR_UNDER_MAINTENANCE);
      ShowBalloon(info, get_weasel_ime_name().c_str());
      m_disabled = true;
    }
    if (m_mode != DISABLED)
      m_disabled = false;
  } else if (!Visible()) {
    ShowIcon();
  }
}
