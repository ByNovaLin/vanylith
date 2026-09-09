#pragma once

#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>

namespace vanityforge {

class WebViewHost;

class DesktopApp final {
 public:
  DesktopApp();
  ~DesktopApp();
  DesktopApp(const DesktopApp&) = delete;
  DesktopApp& operator=(const DesktopApp&) = delete;

  bool initialize(HINSTANCE instance, bool open_devtools, std::string& error);
  int run();

 private:
  static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam,
                                            LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
  void activate();
  void webview_failed(const std::string& message);
  static std::filesystem::path webview_user_data_directory(std::string& error);

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  std::unique_ptr<WebViewHost> webview_;
  bool webview_ready_ = false;
  int exit_code_ = 0;
};

}  // namespace vanityforge
