#pragma once

#include <Windows.h>

#include <string>

namespace vanityforge {

inline constexpr wchar_t desktop_window_class_name[] = L"Vanylith.DesktopWindow.v1";
inline constexpr UINT desktop_activate_message = WM_APP + 17;

enum class SingleInstanceResult {
  primary,
  activated_existing,
  existing_without_window,
  error,
};

class SingleInstance final {
 public:
  SingleInstance() = default;
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  SingleInstanceResult acquire(std::string& error);

 private:
  static bool activate_existing_window();

  HANDLE mutex_ = nullptr;
  bool owns_mutex_ = false;
};

}  // namespace vanityforge
