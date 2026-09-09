#include "test_framework.h"

#include "backend/cuda/cuda_backend.h"
#include "backend/cuda/cuda_test_api.h"
#include "controller/task_manager.h"
#include "core/pattern.h"
#include "core/search_space.h"
#include "core/tron_address.h"
#include "web/api.h"

#include <cuda_runtime.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

vanityforge::PrivateKeyBytes key_from_hex(const std::string& hex) {
  if (hex.size() != 64) throw std::invalid_argument("test key must contain 64 hex characters");
  vanityforge::PrivateKeyBytes result{};
  const auto nibble = [](const char c) -> std::uint8_t {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    throw std::invalid_argument("invalid test hex");
  };
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>((nibble(hex[index * 2]) << 4) |
                                              nibble(hex[index * 2 + 1]));
  }
  return result;
}

void require_rtx_5070_cuda() {
  VF_REQUIRE(vanityforge::cuda_runtime_available_for_tests());
  cudaDeviceProp properties{};
  VF_REQUIRE_EQ(cudaGetDeviceProperties(&properties, 0), cudaSuccess);
  VF_REQUIRE_EQ(properties.major, 12);
  VF_REQUIRE_EQ(properties.minor, 0);
}

std::pair<vanityforge::PrivateKeyBytes, std::string> find_repeat_candidate(
    const vanityforge::PatternPosition position) {
  vanityforge::PatternSpec pattern;
  pattern.mode = vanityforge::PatternMode::repeat;
  pattern.position = position;
  pattern.repeat_prefix_length = 2;
  pattern.repeat_suffix_length = 2;
  const auto one = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  for (std::uint64_t offset = 0; offset < 200000; ++offset) {
    const auto key = vanityforge::candidate_at_offset(one, offset);
    if (!key) break;
    const std::string address = vanityforge::derive_tron_address(*key).base58;
    if (vanityforge::matches_pattern(address, pattern)) return {*key, address};
  }
  throw std::runtime_error("unable to locate deterministic repeat-pattern CUDA test vector");
}

}  // namespace

VF_TEST("CUDA sm_120 derives canonical TRON address vectors on the GPU") {
  require_rtx_5070_cuda();
  const auto one = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  const auto gpu_one = vanityforge::cuda_derive_candidates_for_test(0, one, 0, 1);
  VF_REQUIRE_EQ(gpu_one.size(), static_cast<std::size_t>(1));
  VF_REQUIRE_EQ(gpu_one.front().private_key, one);
  VF_REQUIRE_EQ(gpu_one.front().address, std::string("TMVQGm1qAQYVdetCeGRRkTWYYrLXuHK2HC"));
  VF_REQUIRE_EQ(gpu_one.front().address, vanityforge::derive_tron_address(one).base58);

  const auto ledger = key_from_hex("b5a4cea271ff424d7c31dc12a3e43e401df7a40d7412a15750f3f0b6b5449a28");
  const auto gpu_ledger = vanityforge::cuda_derive_candidates_for_test(0, ledger, 0, 1);
  VF_REQUIRE_EQ(gpu_ledger.size(), static_cast<std::size_t>(1));
  VF_REQUIRE_EQ(gpu_ledger.front().private_key, ledger);
  VF_REQUIRE_EQ(gpu_ledger.front().address, vanityforge::derive_tron_address(ledger).base58);
}

VF_TEST("CUDA scalar offsets and GPU literal matching agree with CPU verification") {
  require_rtx_5070_cuda();
  const auto one = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  constexpr std::uint32_t candidate_count = 65;
  const auto all_candidates =
      vanityforge::cuda_derive_candidates_for_test(0, one, 0, candidate_count);
  VF_REQUIRE_EQ(all_candidates.size(), static_cast<std::size_t>(candidate_count));
  std::map<vanityforge::PrivateKeyBytes, std::string> gpu_by_key;
  for (const auto& candidate : all_candidates) {
    VF_REQUIRE(gpu_by_key.emplace(candidate.private_key, candidate.address).second);
    VF_REQUIRE_EQ(candidate.address, vanityforge::derive_tron_address(candidate.private_key).base58);
  }
  for (std::uint64_t index = 0; index < candidate_count; ++index) {
    const auto expected_key = vanityforge::candidate_at_offset(one, index);
    VF_REQUIRE(expected_key.has_value());
    const auto found = gpu_by_key.find(*expected_key);
    VF_REQUIRE(found != gpu_by_key.end());
    VF_REQUIRE_EQ(found->second, vanityforge::derive_tron_address(*expected_key).base58);
  }

  const auto target_key = vanityforge::candidate_at_offset(one, 2);
  VF_REQUIRE(target_key.has_value());
  const std::string target_address = vanityforge::derive_tron_address(*target_key).base58;
  vanityforge::PatternSpec pattern;
  pattern.mode = vanityforge::PatternMode::literal;
  pattern.position = vanityforge::PatternPosition::both;
  pattern.literal_prefix = target_address.substr(1, 2);
  pattern.literal_suffix = target_address.substr(target_address.size() - 2);
  const auto gpu_matches =
      vanityforge::cuda_search_candidates_for_test(0, one, 0, candidate_count, pattern);
  std::map<vanityforge::PrivateKeyBytes, std::string> expected_matches;
  for (const auto& candidate : all_candidates) {
    if (vanityforge::matches_pattern(candidate.address, pattern)) {
      expected_matches.emplace(candidate.private_key, candidate.address);
    }
  }
  std::map<vanityforge::PrivateKeyBytes, std::string> actual_matches;
  for (const auto& candidate : gpu_matches) {
    VF_REQUIRE(actual_matches.emplace(candidate.private_key, candidate.address).second);
  }
  VF_REQUIRE(!expected_matches.empty());
  VF_REQUIRE_EQ(actual_matches, expected_matches);
}

VF_TEST("CUDA multi-group field arithmetic agrees with independent CPU derivation") {
  require_rtx_5070_cuda();
  constexpr std::uint32_t candidate_count = 129;
  const std::array<const char*, 4> base_key_hex{
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
      "f000000000000000000000000000000000000000000000000000000000000001"};

  for (const char* hex : base_key_hex) {
    const auto base_key = key_from_hex(hex);
    const auto candidates =
        vanityforge::cuda_derive_candidates_for_test(0, base_key, 0, candidate_count);
    VF_REQUIRE_EQ(candidates.size(), static_cast<std::size_t>(candidate_count));
    std::map<vanityforge::PrivateKeyBytes, std::string> gpu_by_key;
    for (const auto& candidate : candidates) {
      VF_REQUIRE(gpu_by_key.emplace(candidate.private_key, candidate.address).second);
      VF_REQUIRE_EQ(candidate.address, vanityforge::derive_tron_address(candidate.private_key).base58);
    }
    for (std::uint64_t offset = 0; offset < candidate_count; ++offset) {
      const auto expected_key = vanityforge::candidate_at_offset(base_key, offset);
      VF_REQUIRE(expected_key.has_value());
      VF_REQUIRE(gpu_by_key.contains(*expected_key));
    }
  }
}

VF_TEST("CUDA rejects curve-order overflow without scalar wraparound") {
  require_rtx_5070_cuda();
  const auto order_minus_one =
      key_from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140");
  const auto bounded = vanityforge::cuda_derive_candidates_for_test(0, order_minus_one, 0, 2);
  VF_REQUIRE_EQ(bounded.size(), static_cast<std::size_t>(1));
  VF_REQUIRE_EQ(bounded.front().private_key, order_minus_one);
  VF_REQUIRE_EQ(bounded.front().address, vanityforge::derive_tron_address(order_minus_one).base58);

  const auto order =
      key_from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  VF_REQUIRE(vanityforge::cuda_derive_candidates_for_test(0, order, 0, 1).empty());
  const auto order_plus_one =
      key_from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364142");
  VF_REQUIRE(vanityforge::cuda_derive_candidates_for_test(0, order_plus_one, 0, 1).empty());
}

VF_TEST("CudaBackend emits a GPU-found candidate that independently CPU-verifies") {
  require_rtx_5070_cuda();
  const auto one = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  vanityforge::SearchTask task;
  task.pattern.mode = vanityforge::PatternMode::literal;
  task.pattern.position = vanityforge::PatternPosition::suffix;
  task.pattern.literal_suffix = "MVQGm1qAQYVdetCeGRRkTWYYrLXuHK2HC";

  vanityforge::CudaBackend backend(0);
  VF_REQUIRE(backend.initialize());
  vanityforge::SearchSpaceAllocator allocator(1);
  auto control = std::make_shared<vanityforge::SearchControl>();
  std::mutex mutex;
  std::condition_variable condition;
  bool found = false;
  std::string callback_address;
  vanityforge::PrivateKeyBytes callback_key{};
  std::string backend_error;
  backend.start(task, one, allocator, control,
                [&](vanityforge::PrivateKeyBytes&& key, const std::string& address) {
                  std::lock_guard lock(mutex);
                  callback_key = key;
                  callback_address = address;
                  found = true;
                  condition.notify_all();
                  return false;
                },
                [&](const std::string& error) {
                  std::lock_guard lock(mutex);
                  backend_error = error;
                  condition.notify_all();
                });
  {
    std::unique_lock lock(mutex);
    VF_REQUIRE(condition.wait_for(lock, std::chrono::seconds(60), [&] {
      return found || !backend_error.empty();
    }));
  }
  backend.stop();
  VF_REQUIRE(backend_error.empty());
  VF_REQUIRE(found);
  VF_REQUIRE_EQ(callback_key, one);
  VF_REQUIRE_EQ(callback_address, vanityforge::derive_tron_address(callback_key).base58);
}

VF_TEST("CUDA matcher agrees with CPU for Literal and Repeat Prefix Suffix Both") {
  require_rtx_5070_cuda();
  const auto one = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  const std::string one_address = vanityforge::derive_tron_address(one).base58;
  const std::array literal_positions{
      vanityforge::PatternPosition::prefix,
      vanityforge::PatternPosition::suffix,
      vanityforge::PatternPosition::both};
  for (const auto position : literal_positions) {
    vanityforge::PatternSpec pattern;
    pattern.mode = vanityforge::PatternMode::literal;
    pattern.position = position;
    pattern.literal_prefix = one_address.substr(1, 3);
    pattern.literal_suffix = one_address.substr(one_address.size() - 3);
    VF_REQUIRE(vanityforge::matches_pattern(one_address, pattern));
    const auto matches = vanityforge::cuda_search_candidates_for_test(0, one, 0, 32, pattern);
    std::map<vanityforge::PrivateKeyBytes, std::string> expected;
    std::map<vanityforge::PrivateKeyBytes, std::string> actual;
    for (std::uint64_t offset = 0; offset < 32; ++offset) {
      const auto key = vanityforge::candidate_at_offset(one, offset);
      VF_REQUIRE(key.has_value());
      const std::string address = vanityforge::derive_tron_address(*key).base58;
      if (vanityforge::matches_pattern(address, pattern)) expected.emplace(*key, address);
    }
    for (const auto& match : matches) actual.emplace(match.private_key, match.address);
    VF_REQUIRE(!expected.empty());
    VF_REQUIRE_EQ(actual, expected);
  }

  const std::array repeat_positions{
      vanityforge::PatternPosition::prefix,
      vanityforge::PatternPosition::suffix,
      vanityforge::PatternPosition::both};
  for (const auto position : repeat_positions) {
    const auto [key, address] = find_repeat_candidate(position);
    vanityforge::PatternSpec pattern;
    pattern.mode = vanityforge::PatternMode::repeat;
    pattern.position = position;
    pattern.repeat_prefix_length = 2;
    pattern.repeat_suffix_length = 2;
    VF_REQUIRE(vanityforge::matches_pattern(address, pattern));
    const auto matches = vanityforge::cuda_search_candidates_for_test(0, key, 0, 32, pattern);
    std::map<vanityforge::PrivateKeyBytes, std::string> expected;
    std::map<vanityforge::PrivateKeyBytes, std::string> actual;
    for (std::uint64_t offset = 0; offset < 32; ++offset) {
      const auto candidate = vanityforge::candidate_at_offset(key, offset);
      VF_REQUIRE(candidate.has_value());
      const std::string candidate_address = vanityforge::derive_tron_address(*candidate).base58;
      if (vanityforge::matches_pattern(candidate_address, pattern)) {
        expected.emplace(*candidate, candidate_address);
      }
    }
    for (const auto& match : matches) actual.emplace(match.private_key, match.address);
    VF_REQUIRE(!expected.empty());
    VF_REQUIRE_EQ(actual, expected);
  }
}

VF_TEST("CUDA and CPU worker ranges stay disjoint through the shared allocator") {
  require_rtx_5070_cuda();
  const auto one = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  vanityforge::SearchSpaceAllocator allocator(47);
  const auto cpu_range = allocator.acquire(11);
  const auto cuda_zero_range = allocator.acquire(17);
  const auto cuda_one_range = allocator.acquire(19);
  VF_REQUIRE(cpu_range.has_value());
  VF_REQUIRE(cuda_zero_range.has_value());
  VF_REQUIRE(cuda_one_range.has_value());
  VF_REQUIRE_EQ(allocator.issued(), static_cast<std::uint64_t>(47));

  std::set<vanityforge::PrivateKeyBytes> seen;
  for (std::uint64_t index = 0; index < cpu_range->count; ++index) {
    const auto key = vanityforge::candidate_at_offset(one, cpu_range->begin_offset + index);
    VF_REQUIRE(key.has_value());
    VF_REQUIRE(seen.insert(*key).second);
  }
  for (const auto& range : {*cuda_zero_range, *cuda_one_range}) {
    const auto candidates = vanityforge::cuda_derive_candidates_for_test(
        0, one, range.begin_offset, static_cast<std::uint32_t>(range.count));
    VF_REQUIRE_EQ(candidates.size(), static_cast<std::size_t>(range.count));
    for (const auto& candidate : candidates) {
      VF_REQUIRE_EQ(candidate.address, vanityforge::derive_tron_address(candidate.private_key).base58);
      VF_REQUIRE(seen.insert(candidate.private_key).second);
    }
  }
  VF_REQUIRE_EQ(seen.size(), static_cast<std::size_t>(47));
}

VF_TEST("CudaBackend Start Pause Resume Stop honors synchronized batch boundaries") {
  require_rtx_5070_cuda();
  const auto one = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  vanityforge::SearchTask task;
  task.pattern.mode = vanityforge::PatternMode::literal;
  task.pattern.position = vanityforge::PatternPosition::suffix;
  task.pattern.literal_suffix = "zzzzzz";
  vanityforge::CudaBackend backend(0);
  VF_REQUIRE(backend.initialize());
  // Keep the task alive even after structural CUDA optimizations push the
  // backend well beyond the original reference throughput.
  vanityforge::SearchSpaceAllocator allocator(std::uint64_t{1} << 40U);
  auto control = std::make_shared<vanityforge::SearchControl>();
  std::mutex error_mutex;
  std::string backend_error;
  backend.start(task, one, allocator, control,
                [](vanityforge::PrivateKeyBytes&&, const std::string&) { return true; },
                [&](const std::string& error) {
                  std::lock_guard lock(error_mutex);
                  backend_error = error;
                });

  const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (backend.get_stats().checked == 0 && std::chrono::steady_clock::now() < start_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  VF_REQUIRE(backend.get_stats().checked > 0);
  backend.pause();
  const auto paused = backend.get_stats();
  VF_REQUIRE(paused.paused);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  VF_REQUIRE_EQ(backend.get_stats().checked, paused.checked);

  backend.resume();
  const auto resume_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (backend.get_stats().checked <= paused.checked &&
         std::chrono::steady_clock::now() < resume_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  VF_REQUIRE(backend.get_stats().checked > paused.checked);
  backend.stop();
  const auto stopped = backend.get_stats();
  VF_REQUIRE(!stopped.running);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  VF_REQUIRE_EQ(backend.get_stats().checked, stopped.checked);
  std::lock_guard lock(error_mutex);
  VF_REQUIRE(backend_error.empty());
}

VF_TEST("TaskManager GPU-only search CPU-verifies saves and reports real CUDA rate") {
  require_rtx_5070_cuda();
  const std::filesystem::path data =
      std::filesystem::path(VANITYFORGE_TEST_DATA_DIR) / "cuda-task-manager";
  vanityforge::TaskManager manager(data);
  std::string error;
  VF_REQUIRE(manager.initialize(error));
  bool found_cuda = false;
  for (const auto& device : manager.snapshot().devices) {
    const bool enable = device.id == "cuda-0";
    if (enable) found_cuda = true;
    VF_REQUIRE(manager.set_device_enabled(device.id, enable, error));
  }
  VF_REQUIRE(found_cuda);

  vanityforge::SearchTask rate_task;
  rate_task.pattern.mode = vanityforge::PatternMode::literal;
  rate_task.pattern.position = vanityforge::PatternPosition::suffix;
  rate_task.pattern.literal_suffix = "zzzzzz";
  VF_REQUIRE(manager.start(rate_task, error));
  bool saw_real_rate = false;
  const auto rate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < rate_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto snapshot = manager.snapshot();
    for (const auto& device : snapshot.devices) {
      if (device.id == "cuda-0" && device.hashrate > 0 && snapshot.total_hashrate > 0) {
        saw_real_rate = true;
      }
    }
    if (saw_real_rate) {
      const std::string json = vanityforge::snapshot_to_json(snapshot);
      VF_REQUIRE(json.find("\"backend\":\"CUDA\"") != std::string::npos);
      break;
    }
  }
  VF_REQUIRE(saw_real_rate);
  VF_REQUIRE(manager.pause(error));
  const auto paused_first = manager.snapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto paused_second = manager.snapshot();
  VF_REQUIRE_EQ(paused_first.status, vanityforge::TaskStatus::paused);
  VF_REQUIRE_EQ(paused_first.checked, paused_second.checked);
  VF_REQUIRE(manager.resume(error));
  const auto resumed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  vanityforge::TaskSnapshot resumed;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    resumed = manager.snapshot();
  } while (resumed.checked <= paused_second.checked &&
           std::chrono::steady_clock::now() < resumed_deadline);
  VF_REQUIRE(resumed.checked > paused_second.checked);
  VF_REQUIRE(manager.stop(error));
  VF_REQUIRE(manager.reset(error));

  vanityforge::SearchTask find_task;
  find_task.pattern.mode = vanityforge::PatternMode::literal;
  find_task.pattern.position = vanityforge::PatternPosition::suffix;
  find_task.pattern.literal_suffix = "1";
  find_task.target_count = 1;
  VF_REQUIRE(manager.start(find_task, error));
  vanityforge::TaskSnapshot completed;
  const auto find_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    completed = manager.snapshot();
  } while (completed.status == vanityforge::TaskStatus::running &&
           std::chrono::steady_clock::now() < find_deadline);
  VF_REQUIRE_EQ(completed.status, vanityforge::TaskStatus::completed);
  VF_REQUIRE_EQ(completed.found_results.size(), static_cast<std::size_t>(1));
  const auto& result = completed.found_results.front();
  VF_REQUIRE_EQ(result.device_id, std::string("cuda-0"));
  VF_REQUIRE_EQ(result.backend, std::string("CUDA"));
  VF_REQUIRE(result.cpu_verified);
  VF_REQUIRE(result.saved_locally);
  VF_REQUIRE(result.address.ends_with('1'));
}

VF_TEST("TaskManager defaults to the strongest GPU with CPU fallback disabled") {
  require_rtx_5070_cuda();
  const std::filesystem::path data =
      std::filesystem::path(VANITYFORGE_TEST_DATA_DIR) / "cuda-default-device";
  vanityforge::TaskManager manager(data);
  std::string error;
  VF_REQUIRE(manager.initialize(error));
  bool found_cuda = false;
  bool found_cpu = false;
  for (const auto& device : manager.snapshot().devices) {
    if (device.id == "cuda-0") {
      found_cuda = true;
      VF_REQUIRE(device.enabled);
    }
    if (device.id == "cpu-0") {
      found_cpu = true;
      VF_REQUIRE(!device.enabled);
    }
  }
  VF_REQUIRE(found_cuda);
  VF_REQUIRE(found_cpu);
}
