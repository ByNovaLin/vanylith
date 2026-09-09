#pragma once

#include "core/pattern.h"

namespace vanityforge {

struct ProbabilityEstimate final {
  long double probability_per_key = 0;
  long double average_trials = 0;
  long double average_seconds = 0;
  long double seconds_50 = 0;
  long double seconds_90 = 0;
  long double seconds_95 = 0;
};

ProbabilityEstimate estimate_probability(const PatternSpec& pattern, long double keys_per_second);

}  // namespace vanityforge

