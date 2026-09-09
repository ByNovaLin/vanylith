#pragma once

#include "backend/backend.h"
#include "controller/task_manager.h"

#include <string>
#include <string_view>

namespace vanityforge {

std::string snapshot_to_json(const TaskSnapshot& snapshot);
bool parse_search_task_json(std::string_view json, SearchTask& task, std::string& error);
bool parse_device_config_json(std::string_view json, std::string& id, bool& enabled,
                              std::string& error);

}  // namespace vanityforge

