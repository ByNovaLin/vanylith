#include "backend/cpu/cpu_backend.h"

#include "core/secure_memory.h"
#include "core/tron_address.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace vanityforge {

CpuBackend::CpuBackend(std::uint32_t thread_count)
    : thread_count_(thread_count == 0 ? std::max(1U, std::thread::hardware_concurrency())
                                      : thread_count) {
  info_.id = "cpu-0";
  info_.kind = BackendKind::cpu;
  info_.threads = thread_count_;
  info_.gpu_index = -1;
}

CpuBackend::~CpuBackend() {
  stop();
  secure_zero(base_key_.data(), base_key_.size());
}

bool CpuBackend::initialize() {
  info_.name = detect_cpu_name();
  return true;
}

void CpuBackend::start(const SearchTask& task, const PrivateKeyBytes& base_key,
                       SearchSpaceAllocator& allocator, std::shared_ptr<SearchControl> control,
                       CandidateCallback candidate_callback,
                       BackendErrorCallback /*error_callback*/) {
  if (!control) throw std::invalid_argument("CPU backend requires a task search control");
  stop();
  task_ = task;
  base_key_ = base_key;
  allocator_ = &allocator;
  control_ = std::move(control);
  callback_ = std::move(candidate_callback);
  checked_.store(0, std::memory_order_relaxed);
  stop_requested_.store(false, std::memory_order_release);
  paused_.store(false, std::memory_order_release);
  active_workers_.store(thread_count_, std::memory_order_release);
  {
    std::lock_guard lock(pause_mutex_);
    paused_workers_ = 0;
  }
  {
    std::lock_guard lock(stats_mutex_);
    last_sample_checked_ = 0;
    last_sample_time_ = std::chrono::steady_clock::now();
    last_hashrate_ = 0;
  }
  threads_.reserve(thread_count_);
  try {
    for (std::uint32_t index = 0; index < thread_count_; ++index) {
      threads_.emplace_back(&CpuBackend::worker_loop, this);
    }
  } catch (...) {
    stop_requested_.store(true, std::memory_order_release);
    pause_condition_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
    threads_.clear();
    active_workers_.store(0, std::memory_order_release);
    secure_zero(base_key_.data(), base_key_.size());
    throw;
  }
}

void CpuBackend::pause() {
  if (!stop_requested_.load(std::memory_order_acquire)) {
    paused_.store(true, std::memory_order_release);
    std::unique_lock lock(pause_mutex_);
    pause_condition_.wait(lock, [this] {
      return stop_requested_.load(std::memory_order_acquire) ||
             paused_workers_ >= active_workers_.load(std::memory_order_acquire);
    });
  }
}

void CpuBackend::resume() {
  paused_.store(false, std::memory_order_release);
  pause_condition_.notify_all();
}

void CpuBackend::stop() {
  stop_requested_.store(true, std::memory_order_release);
  paused_.store(false, std::memory_order_release);
  pause_condition_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) {
      thread.join();
    }
  }
  threads_.clear();
  {
    std::lock_guard lock(pause_mutex_);
    paused_workers_ = 0;
  }
  callback_ = {};
  allocator_ = nullptr;
  secure_zero(base_key_.data(), base_key_.size());
}

BackendStats CpuBackend::get_stats() {
  BackendStats stats;
  stats.checked = checked_.load(std::memory_order_relaxed);
  stats.running = !stop_requested_.load(std::memory_order_acquire);
  stats.paused = paused_.load(std::memory_order_acquire);

  std::lock_guard lock(stats_mutex_);
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - last_sample_time_).count();
  if (stats.running && !stats.paused && seconds >= 0.25) {
    stats.hashrate = static_cast<double>(stats.checked - last_sample_checked_) / seconds;
    last_hashrate_ = stats.hashrate;
    last_sample_checked_ = stats.checked;
    last_sample_time_ = now;
  } else if (stats.running && !stats.paused) {
    stats.hashrate = last_hashrate_;
  }
  return stats;
}

void CpuBackend::worker_loop() {
  constexpr std::uint64_t batch_size = 256;
  std::optional<SearchRange> range;
  std::uint64_t range_index = 0;
  while (!stop_requested_.load(std::memory_order_acquire) &&
         !control_->cancel_requested.load(std::memory_order_acquire)) {
    if (paused_.load(std::memory_order_acquire)) {
      std::unique_lock lock(pause_mutex_);
      ++paused_workers_;
      pause_condition_.notify_all();
      pause_condition_.wait(lock, [this] {
        return stop_requested_.load(std::memory_order_acquire) ||
               control_->cancel_requested.load(std::memory_order_acquire) ||
               !paused_.load(std::memory_order_acquire);
      });
      --paused_workers_;
      pause_condition_.notify_all();
      continue;
    }

    if (!range || range_index >= range->count) {
      range = allocator_->acquire(batch_size);
      range_index = 0;
      if (!range) {
        stop_requested_.store(true, std::memory_order_release);
        break;
      }
    }

    auto candidate = candidate_at_offset(base_key_, range->begin_offset + range_index);
    ++range_index;
    if (!candidate) {
      stop_requested_.store(true, std::memory_order_release);
      break;
    }
    const TronAddress address = derive_tron_address(*candidate);
    checked_.fetch_add(1, std::memory_order_relaxed);
    if (matches_pattern(address.base58, task_.pattern)) {
      if (!callback_(std::move(*candidate), address.base58)) {
        secure_zero(candidate->data(), candidate->size());
        stop_requested_.store(true, std::memory_order_release);
      }
    }
    secure_zero(candidate->data(), candidate->size());
  }
  if (active_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    secure_zero(base_key_.data(), base_key_.size());
  }
  pause_condition_.notify_all();
}

std::string CpuBackend::detect_cpu_name() {
  std::array<char, 256> buffer{};
  DWORD size = static_cast<DWORD>(buffer.size());
  const LSTATUS status = RegGetValueA(HKEY_LOCAL_MACHINE,
                                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                      "ProcessorNameString", RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
  if (status != ERROR_SUCCESS || buffer[0] == '\0') {
    return "Windows CPU";
  }
  std::string name(buffer.data());
  const auto first = name.find_first_not_of(' ');
  const auto last = name.find_last_not_of(' ');
  return first == std::string::npos ? "Windows CPU" : name.substr(first, last - first + 1);
}

}  // namespace vanityforge
