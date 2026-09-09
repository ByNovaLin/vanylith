#include "desktop/webview_host.h"

#include <wrl.h>

#include <iomanip>
#include <sstream>
#include <string_view>

namespace vanityforge {
namespace {

using Microsoft::WRL::Callback;

constexpr wchar_t dashboard_url[] = L"http://127.0.0.1:8787/";
constexpr std::wstring_view allowed_navigation_prefix = dashboard_url;

bool is_allowed_navigation(const wchar_t* uri) {
  if (uri == nullptr) return false;
  const std::wstring_view value(uri);
  return value.starts_with(allowed_navigation_prefix);
}

}  // namespace

WebViewHost::~WebViewHost() { shutdown(); }

bool WebViewHost::runtime_available(std::wstring& version, std::string& error) {
  LPWSTR raw_version = nullptr;
  const HRESULT result = GetAvailableCoreWebView2BrowserVersionString(nullptr, &raw_version);
  if (FAILED(result) || raw_version == nullptr || raw_version[0] == L'\0') {
    if (raw_version != nullptr) CoTaskMemFree(raw_version);
    error = "Microsoft Edge WebView2 Runtime is required but was not found. "
            "Install the Evergreen WebView2 Runtime from Microsoft and start Vanylith again. "
            "https://developer.microsoft.com/microsoft-edge/webview2/";
    return false;
  }
  version.assign(raw_version);
  CoTaskMemFree(raw_version);
  error.clear();
  return true;
}

bool WebViewHost::initialize(HWND parent, const std::filesystem::path& user_data_directory,
                             const bool open_devtools, FailureHandler failure_handler,
                             ReadyHandler ready_handler, std::string& error) {
  parent_ = parent;
  open_devtools_ = open_devtools;
  failure_handler_ = std::move(failure_handler);
  ready_handler_ = std::move(ready_handler);

  const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data_directory.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](const HRESULT callback_result, ICoreWebView2Environment* environment) -> HRESULT {
            environment_created(callback_result, environment);
            return S_OK;
          })
          .Get());
  if (FAILED(result)) {
    error = hresult_message("Unable to begin WebView2 initialization", result);
    return false;
  }
  error.clear();
  return true;
}

void WebViewHost::environment_created(const HRESULT result,
                                      ICoreWebView2Environment* environment) {
  if (failed_ || parent_ == nullptr) return;
  if (FAILED(result) || environment == nullptr) {
    fail(hresult_message("Unable to initialize the WebView2 environment", result));
    return;
  }
  environment_ = environment;
  const HRESULT create_result = environment_->CreateCoreWebView2Controller(
      parent_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [this](const HRESULT callback_result,
                          ICoreWebView2Controller* controller) -> HRESULT {
                     controller_created(callback_result, controller);
                     return S_OK;
                   })
                   .Get());
  if (FAILED(create_result)) {
    fail(hresult_message("Unable to create the WebView2 controller", create_result));
  }
}

void WebViewHost::controller_created(const HRESULT result, ICoreWebView2Controller* controller) {
  if (failed_ || parent_ == nullptr) return;
  if (FAILED(result) || controller == nullptr) {
    fail(hresult_message("Unable to create the WebView2 controller", result));
    return;
  }
  controller_ = controller;
  if (FAILED(controller_->get_CoreWebView2(&webview_)) || webview_ == nullptr) {
    fail("WebView2 did not provide a browser instance.");
    return;
  }

  Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview_->get_Settings(&settings)) && settings != nullptr) {
    settings->put_AreDevToolsEnabled(open_devtools_ ? TRUE : FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
  }

  EventRegistrationToken ignored{};
  webview_->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            if (FAILED(args->get_Uri(&uri))) return S_OK;
            const bool allowed = is_allowed_navigation(uri);
            CoTaskMemFree(uri);
            if (!allowed) args->put_Cancel(TRUE);
            return S_OK;
          })
          .Get(),
      &ignored);
  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            return S_OK;
          })
          .Get(),
      &ignored);
  webview_->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
          })
          .Get(),
      &ignored);
  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            navigation_completed(success == TRUE);
            return S_OK;
          })
          .Get(),
      &ignored);

  controller_->put_IsVisible(FALSE);
  resize();
  navigate();
}

void WebViewHost::navigate() {
  if (webview_ == nullptr || failed_ || ready_) return;
  ++navigation_attempts_;
  const HRESULT result = webview_->Navigate(dashboard_url);
  if (FAILED(result)) navigation_completed(false);
}

void WebViewHost::navigation_completed(const bool success) {
  if (failed_ || ready_) return;
  if (!success) {
    if (navigation_attempts_ < maximum_navigation_attempts) {
      SetTimer(parent_, navigation_retry_timer, 500, nullptr);
      return;
    }
    fail("The local Vanylith Dashboard did not become ready after 10 attempts.");
    return;
  }

  ready_ = true;
  controller_->put_IsVisible(TRUE);
  resize();
  if (open_devtools_) webview_->OpenDevToolsWindow();
  if (ready_handler_) ready_handler_();
}

void WebViewHost::resize() {
  if (controller_ == nullptr || parent_ == nullptr) return;
  RECT bounds{};
  if (GetClientRect(parent_, &bounds)) controller_->put_Bounds(bounds);
}

bool WebViewHost::handle_timer(const UINT_PTR timer_id) {
  if (timer_id != navigation_retry_timer) return false;
  KillTimer(parent_, navigation_retry_timer);
  navigate();
  return true;
}

void WebViewHost::shutdown() {
  if (parent_ != nullptr) KillTimer(parent_, navigation_retry_timer);
  if (controller_ != nullptr) {
    controller_->put_IsVisible(FALSE);
    controller_->Close();
  }
  webview_.Reset();
  controller_.Reset();
  environment_.Reset();
  failure_handler_ = {};
  ready_handler_ = {};
  parent_ = nullptr;
}

void WebViewHost::fail(const std::string& message) {
  if (failed_) return;
  failed_ = true;
  if (parent_ != nullptr) KillTimer(parent_, navigation_retry_timer);
  if (failure_handler_) failure_handler_(message);
}

std::string WebViewHost::hresult_message(const char* operation, const HRESULT result) {
  std::ostringstream message;
  message << operation << " (HRESULT 0x" << std::uppercase << std::hex << std::setw(8)
          << std::setfill('0') << static_cast<unsigned long>(result) << ").";
  return message.str();
}

}  // namespace vanityforge
