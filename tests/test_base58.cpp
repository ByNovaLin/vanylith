#include "test_framework.h"

#include "core/base58.h"

#include <array>
#include <cstdint>
#include <string>

VF_TEST("Base58 preserves leading zero bytes") {
  const std::array<std::uint8_t, 3> input{0, 0, 1};
  VF_REQUIRE_EQ(vanityforge::base58_encode(input), std::string("112"));
}

VF_TEST("Base58 encodes Bitcoin wiki vector") {
  const std::array<std::uint8_t, 6> input{0x00, 0x00, 0x28, 0x7f, 0xb4, 0xcd};
  VF_REQUIRE_EQ(vanityforge::base58_encode(input), std::string("11233QC4"));
}

VF_TEST("Base58 alphabet excludes ambiguous characters") {
  for (const char invalid : std::string("0OIl")) {
    VF_REQUIRE(!vanityforge::is_base58_character(invalid));
  }
  VF_REQUIRE(vanityforge::is_base58_character('1'));
  VF_REQUIRE(vanityforge::is_base58_character('z'));
}

