#include "controller/task_manager.h"
#include "desktop/desktop_app.h"
#include "desktop/single_instance.h"
#include "web/local_server.h"
#include "web/resource.h"

#include <Windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> running{true};

BOOL WINAPI console_handler(const DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    running.store(false, std::memory_order_release);
    const HWND window = FindWindowW(vanityforge::desktop_window_class_name, nullptr);
    if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
  }
  return FALSE;
}

std::vector<std::wstring> command_line_arguments() {
  int count = 0;
  LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::wstring> arguments;
  if (values != nullptr) {
    arguments.assign(values, values + count);
    LocalFree(values);
  }
  return arguments;
}

bool has_argument(const std::vector<std::wstring>& arguments, const std::wstring_view wanted) {
  for (const auto& argument : arguments) if (argument == wanted) return true;
  return false;
}

std::filesystem::path executable_directory() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return std::filesystem::current_path();
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path data_directory(const std::vector<std::wstring>& arguments) {
  for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
    if (arguments[index] == L"--data-dir") return std::filesystem::absolute(arguments[index + 1]);
  }
  return executable_directory();
}

std::string load_binary_resource(const int identifier) {
  HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(identifier),
                                 MAKEINTRESOURCEW(10));
  if (resource == nullptr) return {};
  HGLOBAL loaded = LoadResource(nullptr, resource);
  if (loaded == nullptr) return {};
  const DWORD size = SizeofResource(nullptr, resource);
  const void* bytes = LockResource(loaded);
  if (bytes == nullptr || size == 0) return {};
  return std::string(static_cast<const char*>(bytes), size);
}

void enable_console() {
  if (!AllocConsole()) return;
  FILE* stream = nullptr;
  freopen_s(&stream, "CONOUT$", "w", stdout);
  freopen_s(&stream, "CONOUT$", "w", stderr);
  SetConsoleCtrlHandler(console_handler, TRUE);
}

void append_log(const std::filesystem::path& root, const std::string_view message) {
  std::error_code error;
  std::filesystem::create_directories(root / L"logs", error);
  std::ofstream output(root / L"logs" / L"vanylith.log", std::ios::app);
  if (output) output << message << '\n';
}

int show_startup_error(const bool console, const std::string& error) {
  if (console) std::fprintf(stderr, "Vanylith error: %s\n", error.c_str());
  else MessageBoxA(nullptr, error.c_str(), "Vanylith startup error", MB_OK | MB_ICONERROR);
  return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto arguments = command_line_arguments();
  const bool console = has_argument(arguments, L"--console");
  const bool browser = has_argument(arguments, L"--browser");
  const bool headless = has_argument(arguments, L"--no-browser");
  const bool webview_devtools = has_argument(arguments, L"--webview-devtools");
  if (console) enable_console();

  vanityforge::SingleInstance single_instance;
  std::string error;
  const vanityforge::SingleInstanceResult instance_result = single_instance.acquire(error);
  if (instance_result == vanityforge::SingleInstanceResult::error) {
    return show_startup_error(console, error);
  }
  if (instance_result == vanityforge::SingleInstanceResult::activated_existing) return 0;
  if (instance_result == vanityforge::SingleInstanceResult::existing_without_window) {
    if (console) std::puts("Vanylith is already running.");
    else MessageBoxW(nullptr, L"Vanylith is already running.", L"Vanylith",
                     MB_OK | MB_ICONINFORMATION);
    return 0;
  }

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    return show_startup_error(console, "Unable to initialize the Windows desktop runtime");
  }

  const std::filesystem::path root = data_directory(arguments);
  append_log(root, "Vanylith v0.1.0 starting");
  std::string dashboard = load_binary_resource(IDR_DASHBOARD_HTML);
  std::string horizontal_logo = load_binary_resource(IDR_VANYLITH_HORIZONTAL_SVG);
  std::string icon = load_binary_resource(IDR_VANYLITH_ICON_SVG);
  if (dashboard.empty() || horizontal_logo.empty() || icon.empty()) {
    CoUninitialize();
    return show_startup_error(console, "Embedded Dashboard resource is missing");
  }

  vanityforge::TaskManager task_manager(root);
  if (!task_manager.initialize(error)) {
    CoUninitialize();
    return show_startup_error(console, error);
  }
  append_log(root, "Compute backends initialized");

  vanityforge::LocalServer server(task_manager, std::move(dashboard),
                                  std::move(horizontal_logo), std::move(icon));
  if (!server.start(error)) {
    CoUninitialize();
    return show_startup_error(console, error);
  }
  append_log(root, "Local Web server listening on 127.0.0.1:8787");
  if (console) std::puts("Vanylith READY at http://127.0.0.1:8787");

  int exit_code = 0;
  if (browser) {
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:8787", nullptr,
                                           nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) append_log(root, "Default browser launch failed");
    while (running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  } else if (headless) {
    while (running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  } else {
    vanityforge::DesktopApp desktop;
    if (!desktop.initialize(GetModuleHandleW(nullptr), webview_devtools, error)) {
      exit_code = show_startup_error(console, error);
    } else {
      append_log(root, "WebView2 desktop window started");
      exit_code = desktop.run();
    }
  }

  append_log(root, "Vanylith stopping");
  const vanityforge::TaskStatus status = task_manager.snapshot().status;
  if (status == vanityforge::TaskStatus::running || status == vanityforge::TaskStatus::paused) {
    std::string stop_error;
    if (!task_manager.stop(stop_error)) append_log(root, "Active task shutdown failed: " + stop_error);
  }
  server.stop();
  append_log(root, "Vanylith stopped");
  CoUninitialize();
  return exit_code;
}
