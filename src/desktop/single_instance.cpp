#include "desktop/single_instance.h"

#include <chrono>
#include <thread>

namespace vanityforge {
namespace {

constexpr wchar_t mutex_name[] = L"Local\\Vanylith.MainInstance.v1";
constexpr unsigned int activation_attempts = 100;
constexpr auto activation_interval = std::chrono::milliseconds(100);

}  // namespace

SingleInstance::~SingleInstance() {
  if (owns_mutex_) ReleaseMutex(mutex_);
  if (mutex_ != nullptr) CloseHandle(mutex_);
}

SingleInstanceResult SingleInstance::acquire(std::string& error) {
  mutex_ = CreateMutexW(nullptr, TRUE, mutex_name);
  if (mutex_ == nullptr) {
    error = "Unable to create the single-instance guard (Windows error " +
            std::to_string(GetLastError()) + ")";
    return SingleInstanceResult::error;
  }

  if (GetLastError() != ERROR_ALREADY_EXISTS) {
    owns_mutex_ = true;
    error.clear();
    return SingleInstanceResult::primary;
  }

  CloseHandle(mutex_);
  mutex_ = nullptr;
  if (activate_existing_window()) return SingleInstanceResult::activated_existing;
  return SingleInstanceResult::existing_without_window;
}

bool SingleInstance::activate_existing_window() {
  for (unsigned int attempt = 0; attempt < activation_attempts; ++attempt) {
    const HWND window = FindWindowW(desktop_window_class_name, nullptr);
    if (window != nullptr) {
      DWORD_PTR ignored = 0;
      SendMessageTimeoutW(window, desktop_activate_message, 0, 0,
                          SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &ignored);
      ShowWindowAsync(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(window);
      return true;
    }
    std::this_thread::sleep_for(activation_interval);
  }
  return false;
}

}  // namespace vanityforge
