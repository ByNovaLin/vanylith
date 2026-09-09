#include "test_framework.h"

#include "core/pattern.h"

using vanityforge::PatternMode;
using vanityforge::PatternPosition;
using vanityforge::PatternSpec;

VF_TEST("Literal prefix starts after fixed TRON T") {
  PatternSpec pattern{PatternMode::literal, PatternPosition::prefix, "ABC", "", 5, 5};
  VF_REQUIRE(vanityforge::matches_pattern("TABCxyz123", pattern));
  VF_REQUIRE(!vanityforge::matches_pattern("TABxyz123", pattern));
}

VF_TEST("Literal rejects characters absent from Base58") {
  PatternSpec pattern{PatternMode::literal, PatternPosition::suffix, "", "80OIl", 5, 5};
  VF_REQUIRE(!vanityforge::validate_pattern(pattern).valid);
}

VF_TEST("Repeat prefix excludes fixed TRON T") {
  PatternSpec pattern{PatternMode::repeat, PatternPosition::prefix, "", "", 5, 5};
  VF_REQUIRE(vanityforge::matches_pattern("TAAAAAxyz", pattern));
  VF_REQUIRE(!vanityforge::matches_pattern("TTTTAbxyz", pattern));
}

VF_TEST("Repeat both permits distinct repeated characters") {
  PatternSpec pattern{PatternMode::repeat, PatternPosition::both, "", "", 5, 6};
  VF_REQUIRE(vanityforge::matches_pattern("TAAAAAxyz888888", pattern));
  VF_REQUIRE(!vanityforge::matches_pattern("TAAAAAxyz888887", pattern));
}

