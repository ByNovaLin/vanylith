#pragma once

#include "backend/backend.h"
#include "controller/result_manager.h"
#include "gpu/nvml_monitor.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vanityforge {

enum class TaskStatus { ready, running, paused, completed, error, stopped };

struct PublicResult final {
  std::uint32_t index = 0;
  std::string address;
  std::string prefix;
  std::string suffix;
  std::string found_at;
  std::string device_id = "cpu-0";
  std::string device_name;
  std::string backend = "CPU";
  bool cpu_verified = true;
  bool saved_locally = true;
};

struct DeviceSnapshot final {
  std::string id = "cpu-0";
  std::string name;
  std::string backend = "CPU";
  bool enabled = true;
  double hashrate = 0;
  double base_hashrate = 0;
  std::optional<double> utilization;
  std::optional<double> temperature;
  std::optional<double> power;
  std::optional<double> memory_used;
  std::optional<double> memory_total;
  std::uint32_t threads = 0;
};

struct TaskSnapshot final {
  TaskStatus status = TaskStatus::ready;
  double elapsed = 0;
  std::uint64_t checked = 0;
  double total_hashrate = 0;
  SearchTask task;
  std::vector<DeviceSnapshot> devices;
  std::vector<PublicResult> found_results;
  std::string error;
};

class TaskManager final {
 public:
  explicit TaskManager(const std::filesystem::path& data_directory);
  ~TaskManager();

  bool initialize(std::string& error);
  bool start(const SearchTask& task, std::string& error);
  bool pause(std::string& error);
  bool resume(std::string& error);
  bool stop(std::string& error);
  bool reset(std::string& error);
  bool set_device_enabled(const std::string& id, bool enabled, std::string& error);
  TaskSnapshot snapshot();

 private:
  bool accept_candidate(const DeviceInfo& device, PrivateKeyBytes&& private_key,
                        const std::string& reported_address);
  void report_backend_failure(const DeviceInfo& device, const std::string& message);
  double elapsed_locked(std::chrono::steady_clock::time_point now) const;
  static std::string make_task_id();
  static std::string current_time_text();
  static std::pair<std::string, std::string> matched_parts(const std::string& address,
                                                           const PatternSpec& pattern);

  mutable std::mutex mutex_;
  std::mutex control_mutex_;
  std::vector<std::unique_ptr<SearchBackend>> backends_;
  std::map<std::string, bool> device_enabled_;
  NvmlMonitor nvml_;
  ResultManager results_;
  TaskStatus status_ = TaskStatus::ready;
  SearchTask task_{};
  PrivateKeyBytes base_key_{};
  std::unique_ptr<SearchSpaceAllocator> allocator_;
  std::vector<PublicResult> public_results_;
  std::map<std::string, double> last_hashrate_by_device_;
  std::string task_id_;
  std::string error_;
  std::shared_ptr<SearchControl> search_control_;
  std::chrono::steady_clock::time_point started_at_{};
  std::chrono::steady_clock::time_point paused_at_{};
  std::chrono::steady_clock::time_point finished_at_{};
  std::chrono::steady_clock::duration total_paused_{};
};

const char* task_status_name(TaskStatus status) noexcept;

}  // namespace vanityforge
