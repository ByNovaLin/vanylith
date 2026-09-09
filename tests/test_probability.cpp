#include "test_framework.h"

#include "core/probability.h"

#include <cmath>

using vanityforge::PatternMode;
using vanityforge::PatternPosition;
using vanityforge::PatternSpec;

VF_TEST("Literal eight and repeat eight have different difficulty") {
  PatternSpec literal{PatternMode::literal, PatternPosition::suffix, "", "88888888", 5, 5};
  PatternSpec repeat{PatternMode::repeat, PatternPosition::suffix, "", "", 5, 8};
  const long double literal_space = 1.0L / vanityforge::match_probability(literal);
  const long double repeat_space = 1.0L / vanityforge::match_probability(repeat);
  VF_REQUIRE_NEAR(literal_space / repeat_space, 58.0L, 0.000001L);
}

VF_TEST("Probability quantiles model a stochastic process") {
  PatternSpec literal{PatternMode::literal, PatternPosition::suffix, "", "88", 5, 5};
  const auto estimate = vanityforge::estimate_probability(literal, 1000.0L);
  VF_REQUIRE(estimate.seconds_50 < estimate.seconds_90);
  VF_REQUIRE(estimate.seconds_90 < estimate.seconds_95);
  VF_REQUIRE_NEAR(estimate.average_trials, 58.0L * 58.0L, 0.000001L);
}

