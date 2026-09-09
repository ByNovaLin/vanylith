#include "core/secure_random.h"

#include "core/secure_memory.h"
#include "core/tron_address.h"

#include <Windows.h>
#include <bcrypt.h>

#include <limits>
#include <stdexcept>

namespace vanityforge {

void fill_secure_random(const std::span<std::uint8_t> output) {
  if (output.size() > std::numeric_limits<ULONG>::max()) {
    throw std::invalid_argument("CSPRNG request is too large");
  }
  const NTSTATUS status = BCryptGenRandom(
      nullptr, output.data(), static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    throw std::runtime_error("BCryptGenRandom failed");
  }
}

PrivateKeyBytes generate_private_key() {
  PrivateKeyBytes key{};
  do {
    fill_secure_random(key);
  } while (!is_valid_private_key(key));
  return key;
}

}  // namespace vanityforge

