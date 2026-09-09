#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vanityforge {

struct GpuTelemetry final {
  std::optional<double> utilization;   // percent, 0..100
  std::optional<double> temperature;   // degrees Celsius
  std::optional<double> power;         // watts
  std::optional<double> memory_used;   // GiB
  std::optional<double> memory_total;  // GiB
};

// Dynamically loads the NVIDIA Management Library (nvml.dll) shipped with the
// display driver. Every failure path degrades to empty optionals; NVML being
// absent or permission-limited must never crash or block the search engine.
class NvmlMonitor final {
 public:
  NvmlMonitor();
  ~NvmlMonitor();
  NvmlMonitor(const NvmlMonitor&) = delete;
  NvmlMonitor& operator=(const NvmlMonitor&) = delete;

  bool initialize() noexcept;
  void shutdown() noexcept;

  std::size_t gpu_count() const noexcept;
  std::string gpu_name(std::size_t index) const;
  GpuTelemetry query(std::size_t index) const noexcept;

 private:
  bool initialized_ = false;
  std::size_t gpu_count_ = 0;
  std::vector<std::string> gpu_names_;
  std::vector<void*> gpu_handles_;

  // HMODULE stored as void* so this public header does not need Windows.h.
  // It must outlive every resolved NVML function pointer.
  void* library_ = nullptr;

  // Raw NVML function pointers resolved from nvml.dll.
  int (*fn_init_)() = nullptr;
  int (*fn_shutdown_)() = nullptr;
  int (*fn_count_)(unsigned int*) = nullptr;
  int (*fn_handle_)(unsigned int, void**) = nullptr;
  int (*fn_name_)(void*, char*, unsigned int) = nullptr;
  int (*fn_util_)(void*, void*) = nullptr;
  int (*fn_temp_)(void*, int, unsigned int*) = nullptr;
  int (*fn_power_)(void*, unsigned int*) = nullptr;
  int (*fn_memory_)(void*, void*) = nullptr;

  mutable std::mutex mutex_;
};

}  // namespace vanityforge
