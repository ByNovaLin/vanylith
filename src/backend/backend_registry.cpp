#include "backend/backend_registry.h"

#include "backend/cpu/cpu_backend.h"

namespace vanityforge {

const char* backend_kind_label(const BackendKind kind) noexcept {
  switch (kind) {
    case BackendKind::cpu: return "CPU";
    case BackendKind::cuda: return "CUDA";
    case BackendKind::opencl: return "OpenCL";
  }
  return "CPU";
}

std::vector<std::unique_ptr<SearchBackend>> discover_backends() {
  std::vector<std::unique_ptr<SearchBackend>> backends;
  backends.push_back(std::make_unique<CpuBackend>());
#if defined(VANITYFORGE_ENABLE_CUDA)
  append_cuda_backends(backends);
#endif
#if defined(VANITYFORGE_ENABLE_OPENCL)
  append_opencl_backends(backends);
#endif
  return backends;
}

std::string preferred_default_device_id(const std::vector<DeviceInfo>& devices) {
  const DeviceInfo* preferred_gpu = nullptr;
  const DeviceInfo* preferred_cpu = nullptr;
  for (const auto& device : devices) {
    if (device.kind == BackendKind::cpu) {
      if (preferred_cpu == nullptr) preferred_cpu = &device;
      continue;
    }
    if (preferred_gpu == nullptr || device.performance_score > preferred_gpu->performance_score ||
        (device.performance_score == preferred_gpu->performance_score &&
         device.threads > preferred_gpu->threads)) {
      preferred_gpu = &device;
    }
  }
  if (preferred_gpu != nullptr) return preferred_gpu->id;
  return preferred_cpu == nullptr ? std::string{} : preferred_cpu->id;
}

}  // namespace vanityforge
