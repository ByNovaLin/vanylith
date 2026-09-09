#include "test_framework.h"

#include "gpu/nvml_monitor.h"

#include <iostream>

VF_TEST("NVML monitor degrades gracefully and reports real telemetry when present") {
  vanityforge::NvmlMonitor monitor;
  const bool initialized = monitor.initialize();
  if (!initialized) {
    VF_REQUIRE_EQ(monitor.gpu_count(), static_cast<std::size_t>(0));
    std::cout << "  (NVML unavailable on this host; no NVIDIA GPU telemetry to assert)\n";
    return;
  }

  const std::size_t count = monitor.gpu_count();
  VF_REQUIRE(count >= 1);
  for (std::size_t index = 0; index < count; ++index) {
    const std::string name = monitor.gpu_name(index);
    VF_REQUIRE(!name.empty());

    const auto telemetry = monitor.query(index);
    if (telemetry.utilization) VF_REQUIRE(*telemetry.utilization >= 0.0);
    if (telemetry.temperature) VF_REQUIRE(*telemetry.temperature >= 0.0);
    if (telemetry.power) VF_REQUIRE(*telemetry.power >= 0.0);
    if (telemetry.memory_used && telemetry.memory_total) {
      VF_REQUIRE(*telemetry.memory_total > 0.0);
      VF_REQUIRE(*telemetry.memory_used <= *telemetry.memory_total);
    }
    std::cout << "  GPU[" << index << "] " << name
              << " util=" << (telemetry.utilization ? std::to_string(static_cast<int>(*telemetry.utilization)) + "%" : "-")
              << " temp=" << (telemetry.temperature ? std::to_string(static_cast<int>(*telemetry.temperature)) + "C" : "-")
              << " power=" << (telemetry.power ? std::to_string(static_cast<int>(*telemetry.power)) + "W" : "-")
              << " vram=" << (telemetry.memory_used ? std::to_string(static_cast<int>(*telemetry.memory_used)) + "GiB" : "-")
              << '\n';
  }
  monitor.shutdown();
  VF_REQUIRE_EQ(monitor.gpu_count(), static_cast<std::size_t>(0));

  // Reinitialize after an unload to exercise the DLL ownership boundary.
  VF_REQUIRE(monitor.initialize());
  VF_REQUIRE_EQ(monitor.gpu_count(), count);
  const auto telemetry_after_reinit = monitor.query(0);
  if (telemetry_after_reinit.temperature) VF_REQUIRE(*telemetry_after_reinit.temperature >= 0.0);
  monitor.shutdown();
  VF_REQUIRE_EQ(monitor.gpu_count(), static_cast<std::size_t>(0));
}
