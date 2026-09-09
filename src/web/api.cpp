#include "web/api.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>

namespace vanityforge {
namespace {

std::string json_escape(const std::string_view input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (const unsigned char character : input) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (character < 0x20) {
          constexpr char digits[] = "0123456789abcdef";
          output += "\\u00";
          output.push_back(digits[character >> 4]);
          output.push_back(digits[character & 0x0f]);
        } else {
          output.push_back(static_cast<char>(character));
        }
    }
  }
  return output;
}

std::optional<std::size_t> value_position(const std::string_view json,
                                          const std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(quoted);
  if (key_position == std::string_view::npos) return std::nullopt;
  std::size_t position = json.find(':', key_position + quoted.size());
  if (position == std::string_view::npos) return std::nullopt;
  ++position;
  while (position < json.size() &&
         (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
          json[position] == '\n')) {
    ++position;
  }
  return position;
}

std::optional<std::string> json_string(const std::string_view json, const std::string_view key) {
  const auto start = value_position(json, key);
  if (!start || *start >= json.size() || json[*start] != '"') return std::nullopt;
  std::string output;
  for (std::size_t i = *start + 1; i < json.size(); ++i) {
    const char character = json[i];
    if (character == '"') return output;
    if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
    if (character == '\\') {
      if (++i >= json.size()) return std::nullopt;
      switch (json[i]) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        default: return std::nullopt;
      }
    } else {
      output.push_back(character);
    }
  }
  return std::nullopt;
}

template <typename Integer>
std::optional<Integer> json_integer(const std::string_view json, const std::string_view key) {
  const auto start = value_position(json, key);
  if (!start) return std::nullopt;
  Integer value{};
  const char* begin = json.data() + *start;
  const char* end = json.data() + json.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{}) return std::nullopt;
  return value;
}

std::optional<bool> json_boolean(const std::string_view json, const std::string_view key) {
  const auto start = value_position(json, key);
  if (!start) return std::nullopt;
  if (json.substr(*start, 4) == "true") return true;
  if (json.substr(*start, 5) == "false") return false;
  return std::nullopt;
}

void append_nullable(std::ostringstream& json, const std::optional<double>& value) {
  if (value && std::isfinite(*value)) json << *value;
  else json << "null";
}

}  // namespace

std::string snapshot_to_json(const TaskSnapshot& snapshot) {
  std::ostringstream json;
  json << "{\"status\":\"" << task_status_name(snapshot.status) << "\",\"elapsed\":"
       << snapshot.elapsed << ",\"checked\":" << snapshot.checked << ",\"totalHashrate\":"
       << snapshot.total_hashrate << ",\"error\":\"" << json_escape(snapshot.error)
       << "\",\"task\":{";
  json << "\"matchMode\":\"" << (snapshot.task.pattern.mode == PatternMode::literal ? "specific" : "repeat")
       << "\",\"matchPosition\":\"";
  switch (snapshot.task.pattern.position) {
    case PatternPosition::prefix: json << "prefix"; break;
    case PatternPosition::suffix: json << "suffix"; break;
    case PatternPosition::both: json << "both"; break;
  }
  json << "\",\"specificPrefix\":\"" << json_escape(snapshot.task.pattern.literal_prefix)
       << "\",\"specificSuffix\":\"" << json_escape(snapshot.task.pattern.literal_suffix)
       << "\",\"repeatPrefixLen\":" << snapshot.task.pattern.repeat_prefix_length
       << ",\"repeatSuffixLen\":" << snapshot.task.pattern.repeat_suffix_length
       << ",\"targetCount\":" << snapshot.task.target_count << "},\"devices\":[";

  for (std::size_t index = 0; index < snapshot.devices.size(); ++index) {
    if (index != 0) json << ',';
    const auto& device = snapshot.devices[index];
    json << "{\"id\":\"" << json_escape(device.id) << "\",\"name\":\""
         << json_escape(device.name) << "\",\"backend\":\"" << json_escape(device.backend)
         << "\",\"enabled\":" << (device.enabled ? "true" : "false")
         << ",\"hashrate\":" << device.hashrate << ",\"baseHashrate\":" << device.base_hashrate
         << ",\"utilization\":";
    append_nullable(json, device.utilization);
    json << ",\"temperature\":"; append_nullable(json, device.temperature);
    json << ",\"power\":"; append_nullable(json, device.power);
    json << ",\"memoryUsed\":"; append_nullable(json, device.memory_used);
    json << ",\"memoryTotal\":"; append_nullable(json, device.memory_total);
    json << ",\"threads\":" << device.threads << '}';
  }
  json << "],\"foundResults\":[";
  for (std::size_t index = 0; index < snapshot.found_results.size(); ++index) {
    if (index != 0) json << ',';
    const auto& result = snapshot.found_results[index];
    json << "{\"id\":\"addr-" << result.index << "\",\"index\":" << result.index
         << ",\"address\":\"" << json_escape(result.address) << "\",\"prefix\":\""
         << json_escape(result.prefix) << "\",\"suffix\":\"" << json_escape(result.suffix)
         << "\",\"foundAt\":\"" << json_escape(result.found_at) << "\",\"deviceId\":\""
         << json_escape(result.device_id) << "\",\"deviceName\":\""
         << json_escape(result.device_name) << "\",\"backend\":\""
         << json_escape(result.backend) << "\",\"cpuVerified\":"
         << (result.cpu_verified ? "true" : "false") << ",\"savedLocally\":"
         << (result.saved_locally ? "true" : "false") << '}';
  }
  json << "]}";
  return json.str();
}

bool parse_search_task_json(const std::string_view json, SearchTask& task, std::string& error) {
  const auto mode = json_string(json, "matchMode");
  const auto position = json_string(json, "matchPosition");
  const auto target = json_integer<std::uint32_t>(json, "targetCount");
  if (!mode || !position || !target) {
    error = "Task JSON is missing required fields";
    return false;
  }
  if (*mode == "specific") task.pattern.mode = PatternMode::literal;
  else if (*mode == "repeat") task.pattern.mode = PatternMode::repeat;
  else { error = "Unknown match mode"; return false; }

  if (*position == "prefix") task.pattern.position = PatternPosition::prefix;
  else if (*position == "suffix") task.pattern.position = PatternPosition::suffix;
  else if (*position == "both") task.pattern.position = PatternPosition::both;
  else { error = "Unknown match position"; return false; }

  task.pattern.literal_prefix = json_string(json, "specificPrefix").value_or("");
  task.pattern.literal_suffix = json_string(json, "specificSuffix").value_or("");
  task.pattern.repeat_prefix_length = json_integer<std::size_t>(json, "repeatPrefixLen").value_or(5);
  task.pattern.repeat_suffix_length = json_integer<std::size_t>(json, "repeatSuffixLen").value_or(5);
  task.target_count = *target;
  return true;
}

bool parse_device_config_json(const std::string_view json, std::string& id, bool& enabled,
                              std::string& error) {
  const auto parsed_id = json_string(json, "id");
  const auto parsed_enabled = json_boolean(json, "enabled");
  if (!parsed_id || !parsed_enabled) {
    error = "Device JSON is missing id or enabled";
    return false;
  }
  if (parsed_id->empty()) {
    error = "Device id must not be empty";
    return false;
  }
  id = *parsed_id;
  enabled = *parsed_enabled;
  return true;
}

}  // namespace vanityforge

