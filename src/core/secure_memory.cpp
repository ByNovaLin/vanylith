#include "core/secure_memory.h"

#include <Windows.h>

namespace vanityforge {

void secure_zero(void* memory, const std::size_t size) noexcept {
  if (memory != nullptr && size != 0) {
    SecureZeroMemory(memory, size);
  }
}

}  // namespace vanityforge

