#pragma once

#include "core/types.h"

#include <cstdint>
#include <span>

namespace vanityforge {

Hash256 keccak256(std::span<const std::uint8_t> input) noexcept;

}  // namespace vanityforge

