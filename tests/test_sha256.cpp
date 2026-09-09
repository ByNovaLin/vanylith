#include "test_framework.h"

#include "core/sha256.h"
#include "core/tron_address.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

VF_TEST("Windows CNG SHA-256 matches the abc vector") {
  constexpr std::string_view input = "abc";
  const auto bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  const auto digest = vanityforge::sha256(bytes);
  VF_REQUIRE_EQ(vanityforge::bytes_to_hex(digest.data(), digest.size()),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

