#pragma once

#include "core/types.h"

#include <filesystem>
#include <string>

namespace vanityforge {

class ResultManager final {
 public:
  explicit ResultManager(std::filesystem::path root);
  bool save_wallet(const std::string& task_id, const std::string& address,
                   const PrivateKeyBytes& private_key, std::string& error) const;

 private:
  std::filesystem::path root_;
};

}  // namespace vanityforge

