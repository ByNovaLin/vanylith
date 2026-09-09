#pragma once

#include "backend/backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vanityforge {

// One backend owns one CUDA ordinal and one host-side scheduler thread. CUDA
// types deliberately stay in the .cu implementation so the rest of the
// application remains ordinary C++.
class CudaBackend final : public SearchBackend {
 public:
  explicit CudaBackend(int cuda_ordinal);
  ~CudaBackend() override;

  CudaBackend(const CudaBackend&) = delete;
  CudaBackend& operator=(const CudaBackend&) = delete;

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
  void worker_loop() noexcept;
  bool wait_while_paused();
  void report_error(const std::string& message) noexcept;

  int cuda_ordinal_ = -1;
  DeviceInfo info_;
  SearchTask task_{};
  PrivateKeyBytes base_key_{};
  SearchSpaceAllocator* allocator_ = nullptr;
  std::shared_ptr<SearchControl> control_;
  CandidateCallback candidate_callback_;
  BackendErrorCallback error_callback_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{true};
  std::atomic<bool> paused_{false};
  std::atomic<bool> error_reported_{false};
  std::atomic<std::uint64_t> checked_{0};
  std::condition_variable pause_condition_;
  std::mutex pause_mutex_;
  bool pause_acknowledged_ = false;
  std::mutex stats_mutex_;
  std::uint64_t last_sample_checked_ = 0;
  std::chrono::steady_clock::time_point last_sample_time_{};
  double last_hashrate_ = 0;
};

}  // namespace vanityforge
