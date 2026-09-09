#pragma once

#include "core/pattern.h"
#include "core/search_space.h"
#include "core/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vanityforge {

enum class BackendKind { cpu, cuda, opencl };

struct DeviceInfo final {
  std::string id = "cpu-0";
  std::string name = "Windows CPU";
  BackendKind kind = BackendKind::cpu;
  std::uint32_t threads = 0;
  // A discovery-time capacity proxy used only for the default device choice.
  // CUDA backends use SM count * clock rate; zero keeps CPU opt-in when a GPU exists.
  std::uint64_t performance_score = 0;
  int gpu_index = -1;  // NVML ordinal for GPU backends; -1 for non-GPU devices.
};

const char* backend_kind_label(BackendKind kind) noexcept;

struct SearchTask final {
  PatternSpec pattern;
  std::uint32_t target_count = 1;
};

struct BackendStats final {
  std::uint64_t checked = 0;
  double hashrate = 0;
  bool running = false;
  bool paused = false;
};

// A task-scoped cancellation signal shared by every CPU/GPU backend. It is
// separate from per-backend stop state so a candidate found on one device can
// promptly halt all other devices without allocating overlapping new ranges.
struct SearchControl final {
  std::atomic<bool> cancel_requested{false};
};

using CandidateCallback = std::function<bool(PrivateKeyBytes&&, const std::string&)>;
using BackendErrorCallback = std::function<void(const std::string&)>;

class SearchBackend {
 public:
  virtual ~SearchBackend() = default;
  virtual const DeviceInfo& device_info() const = 0;
  virtual bool initialize() = 0;
  virtual void start(const SearchTask& task, const PrivateKeyBytes& base_key,
                     SearchSpaceAllocator& allocator, std::shared_ptr<SearchControl> control,
                     CandidateCallback candidate_callback,
                     BackendErrorCallback error_callback) = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;
  virtual BackendStats get_stats() = 0;
};

}  // namespace vanityforge
