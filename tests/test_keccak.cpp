#include "test_framework.h"

#include "core/keccak256.h"
#include "core/tron_address.h"

#include <array>
#include <cstdint>
#include <string_view>

VF_TEST("Keccak-256 empty input uses Keccak padding") {
  const std::array<std::uint8_t, 0> input{};
  const auto digest = vanityforge::keccak256(input);
  VF_REQUIRE_EQ(vanityforge::bytes_to_hex(digest.data(), digest.size()),
                std::string("c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"));
}

VF_TEST("Keccak-256 abc standard vector") {
  constexpr std::string_view input = "abc";
  const auto bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  const auto digest = vanityforge::keccak256(bytes);
  VF_REQUIRE_EQ(vanityforge::bytes_to_hex(digest.data(), digest.size()),
                std::string("4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45"));
}

