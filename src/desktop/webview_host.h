#pragma once

#include <Windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <wrl/client.h>

#include <filesystem>
#include <functional>
#include <string>

namespace vanityforge {

class WebViewHost final {
 public:
  using FailureHandler = std::function<void(const std::string&)>;
  using ReadyHandler = std::function<void()>;

  WebViewHost() = default;
  ~WebViewHost();
  WebViewHost(const WebViewHost&) = delete;
  WebViewHost& operator=(const WebViewHost&) = delete;

  static bool runtime_available(std::wstring& version, std::string& error);
  bool initialize(HWND parent, const std::filesystem::path& user_data_directory,
                  bool open_devtools, FailureHandler failure_handler,
                  ReadyHandler ready_handler, std::string& error);
  void resize();
  bool handle_timer(UINT_PTR timer_id);
  void shutdown();

 private:
  static constexpr UINT_PTR navigation_retry_timer = 0x5646;
  static constexpr unsigned int maximum_navigation_attempts = 10;

  void environment_created(HRESULT result, ICoreWebView2Environment* environment);
  void controller_created(HRESULT result, ICoreWebView2Controller* controller);
  void navigate();
  void navigation_completed(bool success);
  void fail(const std::string& message);
  static std::string hresult_message(const char* operation, HRESULT result);

  HWND parent_ = nullptr;
  bool open_devtools_ = false;
  bool failed_ = false;
  bool ready_ = false;
  unsigned int navigation_attempts_ = 0;
  FailureHandler failure_handler_;
  ReadyHandler ready_handler_;
  Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
};

}  // namespace vanityforge
