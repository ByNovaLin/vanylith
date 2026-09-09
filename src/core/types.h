#pragma once

#include <array>
#include <cstdint>

namespace vanityforge {

using PrivateKeyBytes = std::array<std::uint8_t, 32>;
using Hash256 = std::array<std::uint8_t, 32>;
using TronRawAddress = std::array<std::uint8_t, 21>;

}  // namespace vanityforge

