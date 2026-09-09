#pragma once

#include "backend/backend.h"

#include <memory>
#include <vector>

namespace vanityforge {

// Discovers every compute device available on this host and constructs a
// backend for it. The CPU backend is always present. CUDA and OpenCL backends
// are appended only when the corresponding backend was compiled in and its
// runtime is available on this machine.
std::vector<std::unique_ptr<SearchBackend>> discover_backends();

// Selects exactly one default device. GPUs always take precedence over CPU;
// among GPUs the highest discovery-time performance score wins.
std::string preferred_default_device_id(const std::vector<DeviceInfo>& devices);

#if defined(VANITYFORGE_ENABLE_CUDA)
void append_cuda_backends(std::vector<std::unique_ptr<SearchBackend>>& backends);
bool cuda_driver_compatible(std::string& error) noexcept;
#endif

#if defined(VANITYFORGE_ENABLE_OPENCL)
void append_opencl_backends(std::vector<std::unique_ptr<SearchBackend>>& backends);
#endif

}  // namespace vanityforge
