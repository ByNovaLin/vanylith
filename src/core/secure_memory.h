#pragma once

#include <cstddef>

namespace vanityforge {

void secure_zero(void* memory, std::size_t size) noexcept;

}  // namespace vanityforge

