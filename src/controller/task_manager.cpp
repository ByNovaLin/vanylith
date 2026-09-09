#include "controller/task_manager.h"

#include "backend/backend_registry.h"
#include "core/secure_memory.h"
#include "core/secure_random.h"
#include "core/tron_address.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace vanityforge {
namespace {

std::string public_error_message(std::string message) {
  constexpr std::string_view legacy_brand = "VanityForge";
  constexpr std::string_view current_brand = "Vanylith";
  std::size_t position = 0;
  while ((position = message.find(legacy_brand, position)) != std::string::npos) {
    message.replace(position, legacy_brand.size(), current_brand);
    position += current_brand.size();
  }
  return message;
}

}  // namespace

TaskManager::TaskManager(const std::filesystem::path& data_directory)
    : results_(data_directory / L"results") {
  task_.pattern.mode = PatternMode::literal;
  task_.pattern.position = PatternPosition::suffix;
  task_.pattern.literal_suffix = "8888";
  task_.target_count = 5;
}

TaskManager::~TaskManager() {
  if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
  for (auto& backend : backends_) backend->stop();
  secure_zero(base_key_.data(), base_key_.size());
}

bool TaskManager::initialize(std::string& error) {
  std::lock_guard control_lock(control_mutex_);
  if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
  for (auto& backend : backends_) backend->stop();
#if defined(VANITYFORGE_ENABLE_CUDA)
  if (!cuda_driver_compatible(error)) {
    error = public_error_message(std::move(error));
    return false;
  }
#endif
  backends_ = discover_backends();
  device_enabled_.clear();

  std::vector<std::unique_ptr<SearchBackend>> initialized;
  for (auto& backend : backends_) {
    if (!backend->initialize()) continue;
    initialized.push_back(std::move(backend));
  }
  backends_ = std::move(initialized);

  if (backends_.empty()) {
    error = "No compute device could be initialized";
    return false;
  }

  // Product default: use exactly one strongest discovered GPU. Users can
  // still opt into additional GPUs or the CPU fallback from the Dashboard.
  std::vector<DeviceInfo> devices;
  devices.reserve(backends_.size());
  for (const auto& backend : backends_) {
    const DeviceInfo& device = backend->device_info();
    device_enabled_[device.id] = false;
    devices.push_back(device);
  }
  const std::string preferred_id = preferred_default_device_id(devices);
  if (!preferred_id.empty()) device_enabled_[preferred_id] = true;

  nvml_.initialize();
  return true;
}

bool TaskManager::start(const SearchTask& task, std::string& error) {
  const PatternValidation validation = validate_pattern(task.pattern);
  if (!validation.valid) {
    error = validation.error;
    return false;
  }
  if (task.target_count == 0 || task.target_count > 1000) {
    error = "Target count must be between 1 and 1000";
    return false;
  }

  std::lock_guard control_lock(control_mutex_);
  bool any_enabled = false;
  {
    std::lock_guard lock(mutex_);
    if (status_ == TaskStatus::running || status_ == TaskStatus::paused) {
      error = "A task is already active";
      return false;
    }
    any_enabled = std::any_of(backends_.begin(), backends_.end(), [this](const auto& backend) {
      const auto enabled = device_enabled_.find(backend->device_info().id);
      return enabled != device_enabled_.end() && enabled->second;
    });
  }
  if (!any_enabled) {
    error = "No compute device selected";
    return false;
  }
  if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
  for (auto& backend : backends_) backend->stop();

  PrivateKeyBytes generated{};
  try {
    generated = generate_private_key();
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
  PrivateKeyBytes backend_base_key = generated;
  const SearchTask task_copy = task;
  std::shared_ptr<SearchControl> control;
  try {
    control = std::make_shared<SearchControl>();
  } catch (const std::exception& exception) {
    secure_zero(backend_base_key.data(), backend_base_key.size());
    secure_zero(generated.data(), generated.size());
    error = exception.what();
    return false;
  }

  {
    std::lock_guard lock(mutex_);
    task_ = task_copy;
    base_key_ = generated;
    allocator_ = std::make_unique<SearchSpaceAllocator>();
    public_results_.clear();
    last_hashrate_by_device_.clear();
    error_.clear();
    search_control_ = control;
    task_id_ = make_task_id();
    status_ = TaskStatus::running;
    started_at_ = std::chrono::steady_clock::now();
    paused_at_ = {};
    finished_at_ = {};
    total_paused_ = {};
  }
  secure_zero(generated.data(), generated.size());

  try {
    for (auto& backend : backends_) {
      bool enabled = false;
      {
        std::lock_guard lock(mutex_);
        if (status_ != TaskStatus::running) break;
        const auto configured = device_enabled_.find(backend->device_info().id);
        enabled = configured != device_enabled_.end() && configured->second;
      }
      if (!enabled) continue;
      const DeviceInfo device = backend->device_info();
      backend->start(task_copy, backend_base_key, *allocator_, control,
                     [this, device](PrivateKeyBytes&& key, const std::string& address) {
                       return accept_candidate(device, std::move(key), address);
                     },
                     [this, device](const std::string& message) {
                       report_backend_failure(device, message);
                     });
    }
  } catch (const std::exception& exception) {
    {
      std::lock_guard lock(mutex_);
      status_ = TaskStatus::error;
      if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
      finished_at_ = std::chrono::steady_clock::now();
      error_ = exception.what();
      error = error_;
    }
    for (auto& backend : backends_) backend->stop();
    secure_zero(backend_base_key.data(), backend_base_key.size());
    {
      std::lock_guard lock(mutex_);
      secure_zero(base_key_.data(), base_key_.size());
    }
    return false;
  }
  secure_zero(backend_base_key.data(), backend_base_key.size());

  bool should_stop = false;
  {
    std::lock_guard lock(mutex_);
    should_stop = status_ != TaskStatus::running;
  }
  if (should_stop) {
    for (auto& backend : backends_) backend->stop();
  }
  error.clear();
  return true;
}

bool TaskManager::pause(std::string& error) {
  std::lock_guard control_lock(control_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (status_ != TaskStatus::running) {
      error = "Only a running task can be paused";
      return false;
    }
    paused_at_ = std::chrono::steady_clock::now();
    status_ = TaskStatus::paused;
  }
  for (auto& backend : backends_) backend->pause();
  return true;
}

bool TaskManager::resume(std::string& error) {
  std::lock_guard control_lock(control_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (status_ != TaskStatus::paused) {
      error = "Only a paused task can be resumed";
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    total_paused_ += now - paused_at_;
    paused_at_ = {};
    status_ = TaskStatus::running;
  }
  for (auto& backend : backends_) backend->resume();
  return true;
}

bool TaskManager::stop(std::string& error) {
  std::lock_guard control_lock(control_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (status_ != TaskStatus::running && status_ != TaskStatus::paused) {
      error = "No active task to stop";
      return false;
    }
    if (status_ == TaskStatus::paused) {
      total_paused_ += std::chrono::steady_clock::now() - paused_at_;
      paused_at_ = {};
    }
    status_ = TaskStatus::stopped;
    if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
    finished_at_ = std::chrono::steady_clock::now();
  }
  for (auto& backend : backends_) backend->stop();
  {
    std::lock_guard lock(mutex_);
    secure_zero(base_key_.data(), base_key_.size());
  }
  return true;
}

bool TaskManager::reset(std::string& error) {
  std::lock_guard control_lock(control_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
    if (status_ == TaskStatus::running || status_ == TaskStatus::paused) {
      if (status_ == TaskStatus::paused) {
        total_paused_ += std::chrono::steady_clock::now() - paused_at_;
        paused_at_ = {};
      }
      status_ = TaskStatus::stopped;
      finished_at_ = std::chrono::steady_clock::now();
    }
  }
  for (auto& backend : backends_) backend->stop();
  {
    std::lock_guard lock(mutex_);
    secure_zero(base_key_.data(), base_key_.size());
    allocator_.reset();
    search_control_.reset();
    public_results_.clear();
    last_hashrate_by_device_.clear();
    task_id_.clear();
    status_ = TaskStatus::ready;
    error_.clear();
  }
  error.clear();
  return true;
}

bool TaskManager::set_device_enabled(const std::string& id, const bool enabled,
                                     std::string& error) {
  std::lock_guard control_lock(control_mutex_);
  std::lock_guard lock(mutex_);
  if (status_ == TaskStatus::running || status_ == TaskStatus::paused) {
    error = "Device selection is locked while a task is active";
    return false;
  }
  const bool known = std::any_of(backends_.begin(), backends_.end(),
                                 [&id](const auto& backend) { return backend->device_info().id == id; });
  if (!known) {
    error = "Unknown compute device";
    return false;
  }
  device_enabled_[id] = enabled;
  return true;
}

TaskSnapshot TaskManager::snapshot() {
  TaskSnapshot result;
  std::vector<std::pair<DeviceInfo, BackendStats>> device_stats;
  device_stats.reserve(backends_.size());
  for (auto& backend : backends_) {
    device_stats.emplace_back(backend->device_info(), backend->get_stats());
  }

  std::lock_guard lock(mutex_);
  result.status = status_;
  result.elapsed = elapsed_locked(std::chrono::steady_clock::now());
  result.task = task_;
  result.found_results = public_results_;
  result.error = error_;

  std::uint64_t total_checked = 0;
  double total_hashrate = 0;
  for (const auto& [device, stats] : device_stats) {
    const auto configured = device_enabled_.find(device.id);
    const bool enabled = configured != device_enabled_.end() && configured->second;
    if (enabled && result.status != TaskStatus::ready) total_checked += stats.checked;
    double device_hashrate = 0;
    if (status_ == TaskStatus::running && enabled) {
      device_hashrate = stats.hashrate;
    }
    if (device_hashrate > 0) last_hashrate_by_device_[device.id] = device_hashrate;
    total_hashrate += device_hashrate;

    DeviceSnapshot snapshot;
    snapshot.id = device.id;
    snapshot.name = device.name;
    snapshot.backend = backend_kind_label(device.kind);
    snapshot.enabled = enabled;
    snapshot.hashrate = device_hashrate;
    snapshot.base_hashrate = enabled ? last_hashrate_by_device_[device.id] : 0;
    snapshot.threads = device.threads;
    if (device.kind == BackendKind::cuda && device.gpu_index >= 0) {
      const GpuTelemetry telemetry = nvml_.query(static_cast<std::size_t>(device.gpu_index));
      snapshot.utilization = telemetry.utilization;
      snapshot.temperature = telemetry.temperature;
      snapshot.power = telemetry.power;
      snapshot.memory_used = telemetry.memory_used;
      snapshot.memory_total = telemetry.memory_total;
    }
    result.devices.push_back(std::move(snapshot));
  }

  result.checked = total_checked;
  result.total_hashrate = status_ == TaskStatus::running ? total_hashrate : 0;
  return result;
}

bool TaskManager::accept_candidate(const DeviceInfo& device, PrivateKeyBytes&& private_key,
                                   const std::string& reported_address) {
  bool continue_search = false;
  try {
    const TronAddress verified = derive_tron_address(private_key);
    std::lock_guard lock(mutex_);
    if (status_ == TaskStatus::paused) {
      // A candidate already in flight at pause time is discarded, but its
      // worker must remain resumable instead of being inadvertently stopped.
      continue_search = true;
    } else if (status_ != TaskStatus::running || public_results_.size() >= task_.target_count) {
      continue_search = false;
    } else if (verified.base58 != reported_address || !matches_pattern(verified.base58, task_.pattern)) {
      status_ = TaskStatus::error;
      if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
      finished_at_ = std::chrono::steady_clock::now();
      error_ = "CPU verification failed for candidate from " + device.id;
      continue_search = false;
    } else {
      std::string save_error;
      if (!results_.save_wallet(task_id_, verified.base58, private_key, save_error)) {
        status_ = TaskStatus::error;
        if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
        finished_at_ = std::chrono::steady_clock::now();
        error_ = save_error;
        continue_search = false;
      } else {
        const auto [prefix, suffix] = matched_parts(verified.base58, task_.pattern);
        public_results_.insert(public_results_.begin(), PublicResult{
            static_cast<std::uint32_t>(public_results_.size() + 1), verified.base58, prefix, suffix,
            current_time_text(), device.id, device.name, backend_kind_label(device.kind), true, true});
        if (public_results_.size() >= task_.target_count) {
          status_ = TaskStatus::completed;
          if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
          finished_at_ = std::chrono::steady_clock::now();
          secure_zero(base_key_.data(), base_key_.size());
          continue_search = false;
        } else {
          continue_search = true;
        }
      }
    }
  } catch (const std::exception&) {
    std::lock_guard lock(mutex_);
    if (status_ == TaskStatus::running || status_ == TaskStatus::paused) {
      status_ = TaskStatus::error;
      if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
      finished_at_ = std::chrono::steady_clock::now();
      error_ = "CPU verification failed for candidate from " + device.id;
    }
    continue_search = false;
  }
  secure_zero(private_key.data(), private_key.size());
  return continue_search;
}

void TaskManager::report_backend_failure(const DeviceInfo& device, const std::string& message) {
  std::lock_guard lock(mutex_);
  if (status_ != TaskStatus::running && status_ != TaskStatus::paused) return;
  status_ = TaskStatus::error;
  if (search_control_) search_control_->cancel_requested.store(true, std::memory_order_release);
  finished_at_ = std::chrono::steady_clock::now();
  error_ = "Backend " + device.id + " failed: " + public_error_message(message);
}

double TaskManager::elapsed_locked(const std::chrono::steady_clock::time_point now) const {
  if (status_ == TaskStatus::ready) return 0;
  auto effective_now = now;
  if (status_ == TaskStatus::paused) effective_now = paused_at_;
  if ((status_ == TaskStatus::completed || status_ == TaskStatus::error ||
       status_ == TaskStatus::stopped) && finished_at_ != std::chrono::steady_clock::time_point{}) {
    effective_now = finished_at_;
  }
  return std::chrono::duration<double>(effective_now - started_at_ - total_paused_).count();
}

std::string TaskManager::make_task_id() {
  static std::atomic<std::uint32_t> sequence{0};
  const auto system_now = std::chrono::system_clock::now();
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::ostringstream text;
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                system_now.time_since_epoch()).count() % 1000;
  text << "task-" << std::put_time(&local, "%Y%m%d-%H%M%S") << '-'
       << std::setfill('0') << std::setw(3) << milliseconds << '-'
       << std::setw(4) << (sequence.fetch_add(1, std::memory_order_relaxed) % 10000);
  return text.str();
}

std::string TaskManager::current_time_text() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::ostringstream text;
  text << std::put_time(&local, "%H:%M:%S");
  return text.str();
}

std::pair<std::string, std::string> TaskManager::matched_parts(const std::string& address,
                                                               const PatternSpec& pattern) {
  std::string prefix;
  std::string suffix;
  const bool prefix_used = pattern.position == PatternPosition::prefix ||
                           pattern.position == PatternPosition::both;
  const bool suffix_used = pattern.position == PatternPosition::suffix ||
                           pattern.position == PatternPosition::both;
  if (pattern.mode == PatternMode::literal) {
    if (prefix_used) prefix = pattern.literal_prefix;
    if (suffix_used) suffix = pattern.literal_suffix;
  } else {
    if (prefix_used) prefix = address.substr(1, pattern.repeat_prefix_length);
    if (suffix_used) suffix = address.substr(address.size() - pattern.repeat_suffix_length);
  }
  return {prefix, suffix};
}

const char* task_status_name(const TaskStatus status) noexcept {
  switch (status) {
    case TaskStatus::ready: return "READY";
    case TaskStatus::running: return "RUNNING";
    case TaskStatus::paused: return "PAUSED";
    case TaskStatus::completed: return "COMPLETED";
    case TaskStatus::error: return "ERROR";
    case TaskStatus::stopped: return "STOPPED";
  }
  return "ERROR";
}

}  // namespace vanityforge
