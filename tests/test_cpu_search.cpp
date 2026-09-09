#include "test_framework.h"

#include "controller/task_manager.h"
#include "core/pattern.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

void select_cpu_only(vanityforge::TaskManager& manager, std::string& error) {
  bool found_cpu = false;
  for (const auto& device : manager.snapshot().devices) {
    if (device.id == "cpu-0") {
      found_cpu = true;
      VF_REQUIRE(manager.set_device_enabled(device.id, true, error));
    } else {
      VF_REQUIRE(manager.set_device_enabled(device.id, false, error));
    }
  }
  VF_REQUIRE(found_cpu);
}

}  // namespace

VF_TEST("CPU backend completes a real verified vanity search") {
  const std::filesystem::path data = VANITYFORGE_TEST_DATA_DIR;
  vanityforge::TaskManager manager(data);
  std::string error;
  VF_REQUIRE(manager.initialize(error));
  select_cpu_only(manager, error);

  vanityforge::SearchTask task;
  task.pattern.mode = vanityforge::PatternMode::literal;
  task.pattern.position = vanityforge::PatternPosition::suffix;
  task.pattern.literal_suffix = "1";
  task.target_count = 1;
  VF_REQUIRE(manager.start(task, error));

  vanityforge::TaskSnapshot snapshot;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    snapshot = manager.snapshot();
  } while (snapshot.status == vanityforge::TaskStatus::running &&
           std::chrono::steady_clock::now() < deadline);

  VF_REQUIRE_EQ(snapshot.status, vanityforge::TaskStatus::completed);
  VF_REQUIRE_EQ(snapshot.found_results.size(), static_cast<std::size_t>(1));
  VF_REQUIRE_EQ(snapshot.found_results.front().device_id, std::string("cpu-0"));
  VF_REQUIRE_EQ(snapshot.found_results.front().backend, std::string("CPU"));
  VF_REQUIRE(snapshot.found_results.front().address.ends_with('1'));
  VF_REQUIRE(snapshot.found_results.front().cpu_verified);
  VF_REQUIRE(snapshot.found_results.front().saved_locally);
}

VF_TEST("Task manager pauses and resumes an in-progress CPU search") {
  const std::filesystem::path data = std::filesystem::path(VANITYFORGE_TEST_DATA_DIR) / "pause-resume";
  vanityforge::TaskManager manager(data);
  std::string error;
  VF_REQUIRE(manager.initialize(error));
  select_cpu_only(manager, error);

  vanityforge::SearchTask task;
  task.pattern.mode = vanityforge::PatternMode::literal;
  task.pattern.position = vanityforge::PatternPosition::suffix;
  task.pattern.literal_suffix = "zzzzz";
  task.target_count = 1;
  VF_REQUIRE(manager.start(task, error));

  const auto search_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  vanityforge::TaskSnapshot running;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    running = manager.snapshot();
  } while (running.checked == 0 && std::chrono::steady_clock::now() < search_deadline);
  VF_REQUIRE(running.checked > 0);

  VF_REQUIRE(manager.pause(error));
  const auto paused_first = manager.snapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const auto paused_second = manager.snapshot();
  VF_REQUIRE_EQ(paused_first.status, vanityforge::TaskStatus::paused);
  VF_REQUIRE_EQ(paused_first.checked, paused_second.checked);

  VF_REQUIRE(manager.resume(error));
  const auto resume_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  vanityforge::TaskSnapshot resumed;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    resumed = manager.snapshot();
  } while (resumed.checked <= paused_second.checked &&
           std::chrono::steady_clock::now() < resume_deadline);
  VF_REQUIRE(resumed.checked > paused_second.checked);
  VF_REQUIRE(manager.stop(error));
  VF_REQUIRE_EQ(manager.snapshot().status, vanityforge::TaskStatus::stopped);
}
