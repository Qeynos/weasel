#include "stdafx.h"

#include <PipeChannel.h>

using namespace weasel;
using namespace std;
using namespace boost;

namespace {
// 客戶端等待服務端回應的上限。IME 是在宿主程序的 UI 線程上同步調用 IPC 的，
// 沒有上限就意味著服務端一卡住，整個程序（包括 explorer）就永遠卡死。
const DWORD kClientReadTimeoutMs = 3000;

// 消息模式下，只要管道裏有可讀數據，就說明一條完整報文已經到達
// （服務端總是用一次 WriteFile 寫完整條報文），因此可以安全地 ReadFile。
bool _WaitReadable(HANDLE pipe, DWORD timeout_ms) {
  ULONGLONG start = ::GetTickCount64();
  for (;;) {
    DWORD avail = 0;
    if (!::PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL))
      return false;  // 管道已斷開
    if (avail > 0)
      return true;
    if (::GetTickCount64() - start >= timeout_ms)
      return false;
    ::Sleep(1);
  }
}
}  // namespace

#define _ThrowLastError throw ::GetLastError()
// 必須拋出和 catch (DWORD) 完全相同的型別：ERROR_TIMEOUT 這類宏是 long，
// 直接 throw 出去的型別是 long，catch (DWORD) 接不到，異常會一路穿出 TSF
// 回調，Windows 就以 0xc000041d (fatal user callback exception) 終止宿主進程。
#define _ThrowCode(__c) throw static_cast<DWORD>(__c)
#define _ThrowIfNot(__c)                 \
  {                                      \
    DWORD err;                           \
    if ((err = ::GetLastError()) != __c) \
      throw err;                         \
  }

PipeChannelBase::PipeChannelBase(std::wstring&& pn_cmd,
                                 size_t bs = 4 * 1024,
                                 SECURITY_ATTRIBUTES* s = NULL)
    : pname(pn_cmd),
      write_stream(nullptr),
      buff_size(bs),
      buffer(std::make_unique<char[]>(bs)),
      hpipe(INVALID_HANDLE_VALUE),
      has_body(false),
      sa(s) {};

PipeChannelBase::PipeChannelBase(PipeChannelBase&& r)
    : write_stream(std::move(r.write_stream)),
      pname(std::move(r.pname)),
      buff_size(r.buff_size),
      buffer(std::move(r.buffer)),
      hpipe(r.hpipe),
      has_body(r.has_body),
      sa(r.sa) {};

PipeChannelBase::~PipeChannelBase() {
  _FinalizePipe(hpipe);
}

bool PipeChannelBase::_Ensure() {
  try {
    if (_Invalid(hpipe)) {
      hpipe = _Connect(pname.c_str());
      return !_Invalid(hpipe);
    }
  } catch (...) {
    return false;
  }

  return true;
}

HANDLE PipeChannelBase::_Connect(const wchar_t* name) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  while (_Invalid(pipe = _TryConnect()))
    ::WaitNamedPipe(name, 500);
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}

void PipeChannelBase::_Reconnect() {
  _FinalizePipe(hpipe);
  _Ensure();
}

HANDLE PipeChannelBase::_TryConnect() {
  auto pipe = ::CreateFile(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (!_Invalid(pipe)) {
    // connected to the pipe
    return pipe;
  }
  // being busy is not really an error since we just need to wait.
  _ThrowIfNot(ERROR_PIPE_BUSY);
  // All pipe instances are busy
  return INVALID_HANDLE_VALUE;
}

size_t PipeChannelBase::_WritePipe(HANDLE pipe, size_t s, char* b) {
  DWORD lwritten;
  if (!::WriteFile(pipe, b, s, &lwritten, NULL) || lwritten <= 0) {
    _ThrowLastError;
  }
  ::FlushFileBuffers(pipe);
  return lwritten;
}

void PipeChannelBase::_FinalizePipe(HANDLE& p) {
  if (!_Invalid(p)) {
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
  p = INVALID_HANDLE_VALUE;
}

void PipeChannelBase::_Receive(HANDLE pipe, LPVOID msg, size_t rec_len) {
  DWORD lread;
  // 只對客戶端自己的連接設超時；服務端連接線程可以一直等客戶端發消息。
  if (pipe == hpipe && !_WaitReadable(pipe, kClientReadTimeoutMs)) {
    // 報文已經錯位，直接丟棄這條連接，下次請求會自動重連
    _FinalizePipe(hpipe);
    _ThrowCode(ERROR_TIMEOUT);
  }
  BOOL success = ::ReadFile(pipe, msg, rec_len, &lread, NULL);
  if (!success) {
    _ThrowIfNot(ERROR_MORE_DATA);

    memset(buffer.get(), 0, buff_size);
    success = ::ReadFile(pipe, buffer.get(), buff_size, &lread, NULL);
    if (!success) {
      _ThrowLastError;
    }
  }
  has_body = false;
}

HANDLE PipeChannelBase::_ConnectServerPipe(std::wstring& pn) {
  HANDLE pipe =
      CreateNamedPipe(pn.c_str(), PIPE_ACCESS_DUPLEX,
                      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                      PIPE_UNLIMITED_INSTANCES, buff_size, buff_size, 0, sa);
  if (pipe == INVALID_HANDLE_VALUE || !::ConnectNamedPipe(pipe, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}
