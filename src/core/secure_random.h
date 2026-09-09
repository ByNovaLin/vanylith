#pragma once

#include "core/types.h"

#include <cstdint>
#include <span>

namespace vanityforge {

void fill_secure_random(std::span<std::uint8_t> output);
PrivateKeyBytes generate_private_key();

}  // namespace vanityforge

