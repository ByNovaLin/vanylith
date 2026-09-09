#include "test_framework.h"

#include "backend/backend_registry.h"

#include <string>
#include <vector>

VF_TEST("Default device policy enables only the strongest GPU") {
  std::vector<vanityforge::DeviceInfo> devices;
  devices.push_back({"cpu-0", "CPU", vanityforge::BackendKind::cpu, 32, 0, -1});
  devices.push_back({"cuda-0", "Slower GPU", vanityforge::BackendKind::cuda, 84, 120000, 0});
  devices.push_back({"cuda-1", "Faster GPU", vanityforge::BackendKind::cuda, 80, 180000, 1});
  VF_REQUIRE_EQ(vanityforge::preferred_default_device_id(devices), std::string("cuda-1"));
}

VF_TEST("Default device policy falls back to CPU without a GPU") {
  std::vector<vanityforge::DeviceInfo> devices;
  devices.push_back({"cpu-0", "CPU", vanityforge::BackendKind::cpu, 16, 0, -1});
  VF_REQUIRE_EQ(vanityforge::preferred_default_device_id(devices), std::string("cpu-0"));
}

VF_TEST("Default device policy is empty without compute devices") {
  VF_REQUIRE(vanityforge::preferred_default_device_id({}).empty());
}
