#pragma once

#include "backend/backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vanityforge {

class CpuBackend final : public SearchBackend {
 public:
  explicit CpuBackend(std::uint32_t thread_count = 0);
  ~CpuBackend() override;

  CpuBackend(const CpuBackend&) = delete;
  CpuBackend& operator=(const CpuBackend&) = delete;

  bool initialize() override;
  void start(const SearchTask& task, const PrivateKeyBytes& base_key,
             SearchSpaceAllocator& allocator, std::shared_ptr<SearchControl> control,
             CandidateCallback candidate_callback,
             BackendErrorCallback error_callback) override;
  void pause() override;
  void resume() override;
  void stop() override;
  BackendStats get_stats() override;
  const DeviceInfo& device_info() const override { return info_; }

 private:
  void worker_loop();
  static std::string detect_cpu_name();

  std::uint32_t thread_count_;
  DeviceInfo info_;
  SearchTask task_{};
  PrivateKeyBytes base_key_{};
  SearchSpaceAllocator* allocator_ = nullptr;
  std::shared_ptr<SearchControl> control_;
  CandidateCallback callback_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_requested_{true};
  std::atomic<bool> paused_{false};
  std::atomic<std::uint64_t> checked_{0};
  std::atomic<std::uint32_t> active_workers_{0};
  std::condition_variable pause_condition_;
  std::mutex pause_mutex_;
  std::uint32_t paused_workers_ = 0;
  std::mutex stats_mutex_;
  std::uint64_t last_sample_checked_ = 0;
  std::chrono::steady_clock::time_point last_sample_time_{};
  double last_hashrate_ = 0;
};

}  // namespace vanityforge
