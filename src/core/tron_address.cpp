#include "core/tron_address.h"

#include "core/base58.h"
#include "core/keccak256.h"
#include "core/secure_memory.h"
#include "core/secure_random.h"
#include "core/sha256.h"

#include <secp256k1.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <span>
#include <stdexcept>

namespace vanityforge {
namespace {

secp256k1_context* secp_context() {
  static std::once_flag once;
  static secp256k1_context* context = nullptr;
  std::call_once(once, [] {
    context = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (context == nullptr) {
      throw std::runtime_error("Unable to create secp256k1 context");
    }
    std::array<std::uint8_t, 32> randomizer{};
    try {
      fill_secure_random(randomizer);
      if (secp256k1_context_randomize(context, randomizer.data()) != 1) {
        secure_zero(randomizer.data(), randomizer.size());
        throw std::runtime_error("Unable to randomize secp256k1 context");
      }
      secure_zero(randomizer.data(), randomizer.size());
    } catch (...) {
      secure_zero(randomizer.data(), randomizer.size());
      secp256k1_context_destroy(context);
      context = nullptr;
      throw;
    }
  });
  return context;
}

}  // namespace

bool is_valid_private_key(const PrivateKeyBytes& private_key) noexcept {
  try {
    return secp256k1_ec_seckey_verify(secp_context(), private_key.data()) == 1;
  } catch (...) {
    return false;
  }
}

TronAddress derive_tron_address(const PrivateKeyBytes& private_key) {
  if (!is_valid_private_key(private_key)) {
    throw std::invalid_argument("Private key must satisfy 1 <= key < secp256k1 curve order");
  }

  secp256k1_pubkey public_key{};
  if (secp256k1_ec_pubkey_create(secp_context(), &public_key, private_key.data()) != 1) {
    throw std::runtime_error("secp256k1 public key derivation failed");
  }

  std::array<std::uint8_t, 65> serialized{};
  std::size_t serialized_size = serialized.size();
  if (secp256k1_ec_pubkey_serialize(secp_context(), serialized.data(), &serialized_size,
                                    &public_key, SECP256K1_EC_UNCOMPRESSED) != 1 ||
      serialized_size != serialized.size() || serialized[0] != 0x04) {
    throw std::runtime_error("secp256k1 uncompressed serialization failed");
  }

  const Hash256 digest = keccak256(std::span<const std::uint8_t>(serialized).subspan(1));
  TronAddress result;
  result.raw[0] = 0x41;
  std::copy(digest.end() - 20, digest.end(), result.raw.begin() + 1);

  const Hash256 first_checksum = sha256(result.raw);
  const Hash256 second_checksum = sha256(first_checksum);
  std::array<std::uint8_t, 25> checked{};
  std::copy(result.raw.begin(), result.raw.end(), checked.begin());
  std::copy_n(second_checksum.begin(), 4, checked.begin() + result.raw.size());
  result.base58 = base58_encode(checked);

  secure_zero(serialized.data(), serialized.size());
  return result;
}

std::string bytes_to_hex(const std::uint8_t* data, const std::size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (std::size_t i = 0; i < size; ++i) {
    result[i * 2] = digits[data[i] >> 4];
    result[i * 2 + 1] = digits[data[i] & 0x0f];
  }
  return result;
}

}  // namespace vanityforge
