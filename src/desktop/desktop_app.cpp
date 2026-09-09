#include "desktop/desktop_app.h"

#include "desktop/single_instance.h"
#include "desktop/webview_host.h"
#include "web/resource.h"

#include <ShlObj.h>

#include <algorithm>

namespace vanityforge {
namespace {

constexpr wchar_t window_title[] = L"Vanylith";
constexpr int initial_client_width = 1440;
constexpr int initial_client_height = 900;
constexpr int minimum_client_width = 1024;
constexpr int minimum_client_height = 700;

}  // namespace

DesktopApp::DesktopApp() = default;
DesktopApp::~DesktopApp() = default;

bool DesktopApp::initialize(HINSTANCE instance, const bool open_devtools, std::string& error) {
  instance_ = instance;

  std::wstring runtime_version;
  if (!WebViewHost::runtime_available(runtime_version, error)) return false;
  const std::filesystem::path user_data = webview_user_data_directory(error);
  if (user_data.empty()) return false;

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = window_procedure;
  window_class.hInstance = instance_;
  window_class.hIcon = static_cast<HICON>(LoadImageW(
      instance_, MAKEINTRESOURCEW(IDI_VANYLITH_ICON), IMAGE_ICON,
      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = desktop_window_class_name;
  window_class.hIconSm = static_cast<HICON>(LoadImageW(
      instance_, MAKEINTRESOURCEW(IDI_VANYLITH_ICON), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  if (window_class.hIcon == nullptr || window_class.hIconSm == nullptr) {
    error = "Unable to load the Vanylith application icon.";
    return false;
  }
  if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    error = "Unable to register the Vanylith desktop window class.";
    return false;
  }

  const UINT dpi = GetDpiForSystem();
  RECT bounds{0, 0, MulDiv(initial_client_width, static_cast<int>(dpi), 96),
              MulDiv(initial_client_height, static_cast<int>(dpi), 96)};
  AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const LONG width = std::min<LONG>(bounds.right - bounds.left,
                                    work_area.right - work_area.left);
  const LONG height = std::min<LONG>(bounds.bottom - bounds.top,
                                     work_area.bottom - work_area.top);
  const LONG x = work_area.left +
                 std::max<LONG>(0, (work_area.right - work_area.left - width) / 2);
  const LONG y = work_area.top +
                 std::max<LONG>(0, (work_area.bottom - work_area.top - height) / 2);

  window_ = CreateWindowExW(0, desktop_window_class_name, window_title, WS_OVERLAPPEDWINDOW,
                            x, y, width, height, nullptr, nullptr, instance_, this);
  if (window_ == nullptr) {
    error = "Unable to create the Vanylith desktop window.";
    return false;
  }
  SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(window_class.hIcon));
  SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(window_class.hIconSm));

  webview_ = std::make_unique<WebViewHost>();
  if (!webview_->initialize(
          window_, user_data, open_devtools,
          [this](const std::string& message) { webview_failed(message); },
          [this]() {
            webview_ready_ = true;
            InvalidateRect(window_, nullptr, TRUE);
          },
          error)) {
    DestroyWindow(window_);
    window_ = nullptr;
    return false;
  }

  ShowWindow(window_, SW_SHOWNORMAL);
  UpdateWindow(window_);
  error.clear();
  return true;
}

int DesktopApp::run() {
  MSG message{};
  while (true) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == 0) break;
    if (result == -1) {
      exit_code_ = 1;
      break;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return exit_code_;
}

LRESULT CALLBACK DesktopApp::window_procedure(const HWND window, const UINT message,
                                               const WPARAM wparam, const LPARAM lparam) {
  DesktopApp* app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    app = static_cast<DesktopApp*>(create->lpCreateParams);
    app->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (app != nullptr) return app->handle_message(message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT DesktopApp::handle_message(const UINT message, const WPARAM wparam,
                                   const LPARAM lparam) {
  switch (message) {
    case desktop_activate_message:
      activate();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      const UINT dpi = GetDpiForWindow(window_);
      RECT minimum{0, 0, MulDiv(minimum_client_width, static_cast<int>(dpi), 96),
                   MulDiv(minimum_client_height, static_cast<int>(dpi), 96)};
      AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
      limits->ptMinTrackSize.x = minimum.right - minimum.left;
      limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
      return 0;
    }
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      return 0;
    }
    case WM_SIZE:
      if (webview_ != nullptr) webview_->resize();
      return 0;
    case WM_TIMER:
      if (webview_ != nullptr && webview_->handle_timer(wparam)) return 0;
      break;
    case WM_ERASEBKGND:
      if (!webview_ready_) return 1;
      break;
    case WM_PAINT:
      if (!webview_ready_) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(device, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, RGB(45, 55, 72));
        DrawTextW(device, L"Starting Vanylith...", -1, &client,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window_, &paint);
        return 0;
      }
      break;
    case WM_CLOSE:
      DestroyWindow(window_);
      return 0;
    case WM_DESTROY:
      if (webview_ != nullptr) webview_->shutdown();
      window_ = nullptr;
      PostQuitMessage(exit_code_);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

void DesktopApp::activate() {
  if (window_ == nullptr) return;
  if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
  else ShowWindow(window_, SW_SHOW);
  SetForegroundWindow(window_);
}

void DesktopApp::webview_failed(const std::string& message) {
  exit_code_ = 1;
  MessageBoxA(window_, message.c_str(), "Vanylith WebView2 error", MB_OK | MB_ICONERROR);
  if (window_ != nullptr) DestroyWindow(window_);
}

std::filesystem::path DesktopApp::webview_user_data_directory(std::string& error) {
  PWSTR local_app_data = nullptr;
  const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                               &local_app_data);
  if (FAILED(result) || local_app_data == nullptr) {
    if (local_app_data != nullptr) CoTaskMemFree(local_app_data);
    error = "Unable to locate the per-user application data directory for WebView2.";
    return {};
  }
  std::filesystem::path path(local_app_data);
  CoTaskMemFree(local_app_data);
  path /= L"Vanylith";
  path /= L"WebView2";
  error.clear();
  return path;
}

}  // namespace vanityforge
