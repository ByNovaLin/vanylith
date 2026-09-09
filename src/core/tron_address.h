#pragma once

#include "core/types.h"

#include <string>

namespace vanityforge {

struct TronAddress final {
  TronRawAddress raw{};
  std::string base58;
};

bool is_valid_private_key(const PrivateKeyBytes& private_key) noexcept;
TronAddress derive_tron_address(const PrivateKeyBytes& private_key);
std::string bytes_to_hex(const std::uint8_t* data, std::size_t size);

}  // namespace vanityforge

