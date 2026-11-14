#pragma once
#include <ResponseParser.h>
#include <functional>

namespace weasel {

template <typename T>
void TryDeserialize(boost::archive::text_wiarchive& ia, T& t) {
  try {
    ia >> t;
  } catch (const boost::archive::archive_exception& e) {
    // 不要彈模態對話框：這段代碼跑在宿主程序的按鍵處理線程裏，
    // 彈窗會把用戶正在使用的程序卡住。只在調試器輸出裏留個記錄。
    std::string msg =
        std::string("weasel ipc: boost::archive::archive_exception: ") +
        e.what();
    OutputDebugStringA(msg.c_str());
  } catch (...) {
    // 殘缺/錯位的報文可能拋出 length_error / bad_alloc 之類的異常，
    // 這裡必須兜住，否則異常會穿出 IME 回調直接終結宿主程序。
    OutputDebugStringA("weasel ipc: failed to deserialize response");
  }
}
class Deserializer {
 public:
  typedef std::vector<std::wstring> KeyType;
  typedef std::shared_ptr<Deserializer> Ptr;
  typedef std::function<Ptr(ResponseParser* pTarget)> Factory;

  Deserializer(ResponseParser* pTarget) : m_pTarget(pTarget) {}
  virtual ~Deserializer() {}
  virtual void Store(KeyType const& key, std::wstring const& value) {}

  static void Initialize(ResponseParser* pTarget);
  static void Define(std::wstring const& action, Factory factory);
  static bool Require(std::wstring const& action, ResponseParser* pTarget);

 protected:
  ResponseParser* m_pTarget;

 private:
  static std::map<std::wstring, Factory> s_factories;
};

}  // namespace weasel
