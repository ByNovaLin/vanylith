#include "test_framework.h"

#include "core/tron_address.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

vanityforge::PrivateKeyBytes key_from_hex(const std::string& hex) {
  if (hex.size() != 64) throw std::invalid_argument("test key must contain 64 hex characters");
  vanityforge::PrivateKeyBytes result{};
  const auto nibble = [](const char c) -> std::uint8_t {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    throw std::invalid_argument("invalid test hex");
  };
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<std::uint8_t>((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
  }
  return result;
}

}  // namespace

VF_TEST("Private key range rejects zero and curve order") {
  vanityforge::PrivateKeyBytes zero{};
  VF_REQUIRE(!vanityforge::is_valid_private_key(zero));
  const auto order = key_from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  VF_REQUIRE(!vanityforge::is_valid_private_key(order));
}

VF_TEST("Private key one derives known TRON mainnet address") {
  auto key = key_from_hex("0000000000000000000000000000000000000000000000000000000000000001");
  const auto address = vanityforge::derive_tron_address(key);
  VF_REQUIRE_EQ(vanityforge::bytes_to_hex(address.raw.data(), address.raw.size()),
                std::string("417e5f4552091a69125d5dfcb7b8c2659029395bdf"));
  VF_REQUIRE_EQ(address.base58, std::string("TMVQGm1qAQYVdetCeGRRkTWYYrLXuHK2HC"));
}

VF_TEST("Official Ledger TRON vector derives expected raw address") {
  auto key = key_from_hex("b5a4cea271ff424d7c31dc12a3e43e401df7a40d7412a15750f3f0b6b5449a28");
  const auto address = vanityforge::derive_tron_address(key);
  VF_REQUIRE_EQ(vanityforge::bytes_to_hex(address.raw.data(), address.raw.size()),
                std::string("41c8599111f29c1e1e061265b4af93ea1f274ad78a"));
}

