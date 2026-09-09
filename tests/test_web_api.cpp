#include "test_framework.h"

#include "web/api.h"

#include <string>

VF_TEST("Web snapshot contains public result without secret fields") {
  vanityforge::TaskSnapshot snapshot;
  snapshot.task.pattern.literal_suffix = "1";
  snapshot.task.target_count = 1;
  snapshot.devices.push_back(vanityforge::DeviceSnapshot{
      "cuda-0", "NVIDIA GeForce RTX 5070", "CUDA", true,
      753022.0, 753022.0, 98.0, 60.0, 85.0, 1.75, 11.94, 48});
  snapshot.found_results.push_back(vanityforge::PublicResult{
      1, "TMVQGm1qAQYVdetCeGRRkTWYYrLXuHK2HC", "", "C", "12:34:56",
      "cpu-0", "Test CPU", "CPU", true, true});
  const std::string json = vanityforge::snapshot_to_json(snapshot);
  VF_REQUIRE(json.find("TMVQGm1qAQYVdetCeGRRkTWYYrLXuHK2HC") != std::string::npos);
  VF_REQUIRE(json.find("privateKey") == std::string::npos);
  VF_REQUIRE(json.find("private_key") == std::string::npos);
  VF_REQUIRE(json.find("secretKey") == std::string::npos);
  VF_REQUIRE(json.find("mnemonic") == std::string::npos);
  VF_REQUIRE(json.find("seedPhrase") == std::string::npos);
  VF_REQUIRE(json.find("NVIDIA GeForce RTX 5070") != std::string::npos);
  VF_REQUIRE(json.find("\"backend\":\"CUDA\"") != std::string::npos);
  VF_REQUIRE(json.find("\"hashrate\":753022") != std::string::npos);
  VF_REQUIRE(json.find("\"utilization\":98") != std::string::npos);
  VF_REQUIRE(json.find("\"temperature\":60") != std::string::npos);
  VF_REQUIRE(json.find("\"power\":85") != std::string::npos);
  VF_REQUIRE(json.find("\"memoryUsed\":1.75") != std::string::npos);
  VF_REQUIRE(json.find("\"memoryTotal\":11.94") != std::string::npos);
}

VF_TEST("Task API parses repeat-both semantics independently") {
  constexpr std::string_view json =
      R"({"matchMode":"repeat","matchPosition":"both","specificPrefix":"","specificSuffix":"","repeatPrefixLen":5,"repeatSuffixLen":6,"targetCount":2})";
  vanityforge::SearchTask task;
  std::string error;
  VF_REQUIRE(vanityforge::parse_search_task_json(json, task, error));
  VF_REQUIRE_EQ(task.pattern.mode, vanityforge::PatternMode::repeat);
  VF_REQUIRE_EQ(task.pattern.position, vanityforge::PatternPosition::both);
  VF_REQUIRE_EQ(task.pattern.repeat_prefix_length, static_cast<std::size_t>(5));
  VF_REQUIRE_EQ(task.pattern.repeat_suffix_length, static_cast<std::size_t>(6));
  VF_REQUIRE_EQ(task.target_count, static_cast<std::uint32_t>(2));
}
