#pragma once

#include "core/types.h"

#include <cstdint>
#include <span>

namespace vanityforge {

Hash256 sha256(std::span<const std::uint8_t> input);

}  // namespace vanityforge

