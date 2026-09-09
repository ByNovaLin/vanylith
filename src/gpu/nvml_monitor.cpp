#include "gpu/nvml_monitor.h"

#include <Windows.h>

#include <cstring>
#include <utility>

namespace vanityforge {
namespace {

// Minimal NVML ABI declarations. These mirror the public nvml.h layout and are
// resolved dynamically so the executable has no link-time dependency on the
// CUDA toolkit; only the display driver's nvml.dll is required at runtime.
constexpr int kSuccess = 0;

struct NvmlUtilization {
  unsigned int gpu;
  unsigned int memory;
};

struct NvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};

constexpr unsigned int kNameBufferSize = 256;

}  // namespace

NvmlMonitor::NvmlMonitor() = default;

NvmlMonitor::~NvmlMonitor() { shutdown(); }

bool NvmlMonitor::initialize() noexcept {
  std::lock_guard lock(mutex_);
  if (initialized_) return true;

  const HMODULE library = LoadLibraryW(L"nvml.dll");
  if (library == nullptr) return false;

  const auto fn_init = reinterpret_cast<int (*)()>(GetProcAddress(library, "nvmlInit_v2"));
  const auto fn_shutdown = reinterpret_cast<int (*)()>(GetProcAddress(library, "nvmlShutdown"));
  const auto fn_count = reinterpret_cast<int (*)(unsigned int*)>(
      GetProcAddress(library, "nvmlDeviceGetCount_v2"));
  const auto fn_handle = reinterpret_cast<int (*)(unsigned int, void**)>(
      GetProcAddress(library, "nvmlDeviceGetHandleByIndex_v2"));
  const auto fn_name = reinterpret_cast<int (*)(void*, char*, unsigned int)>(
      GetProcAddress(library, "nvmlDeviceGetName"));
  const auto fn_util = reinterpret_cast<int (*)(void*, void*)>(
      GetProcAddress(library, "nvmlDeviceGetUtilizationRates"));
  const auto fn_temp = reinterpret_cast<int (*)(void*, int, unsigned int*)>(
      GetProcAddress(library, "nvmlDeviceGetTemperature"));
  const auto fn_power = reinterpret_cast<int (*)(void*, unsigned int*)>(
      GetProcAddress(library, "nvmlDeviceGetPowerUsage"));
  const auto fn_memory = reinterpret_cast<int (*)(void*, void*)>(
      GetProcAddress(library, "nvmlDeviceGetMemoryInfo"));

  if (fn_init == nullptr || fn_shutdown == nullptr || fn_count == nullptr ||
      fn_handle == nullptr || fn_name == nullptr) {
    FreeLibrary(library);
    return false;
  }

  if (fn_init() != kSuccess) {
    FreeLibrary(library);
    return false;
  }

  unsigned int count = 0;
  if (fn_count(&count) != kSuccess || count == 0) {
    fn_shutdown();
    FreeLibrary(library);
    return false;
  }

  std::vector<void*> handles;
  std::vector<std::string> names;
  try {
    handles.reserve(count);
    names.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
      void* handle = nullptr;
      if (fn_handle(index, &handle) != kSuccess || handle == nullptr) {
        continue;
      }
      char name[kNameBufferSize]{};
      std::string device_name = "NVIDIA GPU";
      if (fn_name(handle, name, kNameBufferSize) == kSuccess && name[0] != '\0') {
        device_name = name;
      }
      handles.push_back(handle);
      names.push_back(std::move(device_name));
    }
  } catch (...) {
    fn_shutdown();
    FreeLibrary(library);
    return false;
  }

  if (handles.empty()) {
    fn_shutdown();
    FreeLibrary(library);
    return false;
  }

  library_ = library;
  fn_init_ = fn_init;
  fn_shutdown_ = fn_shutdown;
  fn_count_ = fn_count;
  fn_handle_ = fn_handle;
  fn_name_ = fn_name;
  fn_util_ = fn_util;
  fn_temp_ = fn_temp;
  fn_power_ = fn_power;
  fn_memory_ = fn_memory;
  gpu_handles_ = std::move(handles);
  gpu_names_ = std::move(names);
  gpu_count_ = gpu_handles_.size();
  initialized_ = true;
  return true;
}

void NvmlMonitor::shutdown() noexcept {
  std::lock_guard lock(mutex_);
  const HMODULE library = static_cast<HMODULE>(library_);
  if (initialized_ && fn_shutdown_ != nullptr) fn_shutdown_();
  fn_init_ = nullptr;
  fn_shutdown_ = nullptr;
  fn_count_ = nullptr;
  fn_handle_ = nullptr;
  fn_name_ = nullptr;
  fn_util_ = nullptr;
  fn_temp_ = nullptr;
  fn_power_ = nullptr;
  fn_memory_ = nullptr;
  gpu_handles_.clear();
  gpu_names_.clear();
  gpu_count_ = 0;
  initialized_ = false;
  library_ = nullptr;
  if (library != nullptr) FreeLibrary(library);
}

std::size_t NvmlMonitor::gpu_count() const noexcept {
  std::lock_guard lock(mutex_);
  return gpu_count_;
}

std::string NvmlMonitor::gpu_name(const std::size_t index) const {
  std::lock_guard lock(mutex_);
  if (index >= gpu_names_.size()) return {};
  return gpu_names_[index];
}

GpuTelemetry NvmlMonitor::query(const std::size_t index) const noexcept {
  GpuTelemetry telemetry;
  std::lock_guard lock(mutex_);
  if (!initialized_ || index >= gpu_handles_.size()) return telemetry;
  void* handle = gpu_handles_[index];

  if (fn_util_ != nullptr) {
    NvmlUtilization utilization{};
    if (fn_util_(handle, &utilization) == kSuccess) {
      telemetry.utilization = static_cast<double>(utilization.gpu);
    }
  }
  if (fn_temp_ != nullptr) {
    unsigned int temperature = 0;
    if (fn_temp_(handle, 0, &temperature) == kSuccess) {
      telemetry.temperature = static_cast<double>(temperature);
    }
  }
  if (fn_power_ != nullptr) {
    unsigned int milliwatts = 0;
    if (fn_power_(handle, &milliwatts) == kSuccess) {
      telemetry.power = static_cast<double>(milliwatts) / 1000.0;
    }
  }
  if (fn_memory_ != nullptr) {
    NvmlMemory memory{};
    if (fn_memory_(handle, &memory) == kSuccess) {
      constexpr double gib = 1024.0 * 1024.0 * 1024.0;
      telemetry.memory_used = static_cast<double>(memory.used) / gib;
      telemetry.memory_total = static_cast<double>(memory.total) / gib;
    }
  }
  return telemetry;
}

}  // namespace vanityforge
