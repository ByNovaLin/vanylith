#include "core/probability.h"

#include <cmath>
#include <limits>

namespace vanityforge {

ProbabilityEstimate estimate_probability(const PatternSpec& pattern,
                                           const long double keys_per_second) {
  ProbabilityEstimate result;
  result.probability_per_key = match_probability(pattern);
  if (result.probability_per_key <= 0.0L) {
    return result;
  }

  result.average_trials = 1.0L / result.probability_per_key;
  if (keys_per_second <= 0.0L) {
    const long double infinity = std::numeric_limits<long double>::infinity();
    result.average_seconds = infinity;
    result.seconds_50 = infinity;
    result.seconds_90 = infinity;
    result.seconds_95 = infinity;
    return result;
  }

  result.average_seconds = result.average_trials / keys_per_second;
  const long double rate = keys_per_second * result.probability_per_key;
  result.seconds_50 = -std::log(1.0L - 0.50L) / rate;
  result.seconds_90 = -std::log(1.0L - 0.90L) / rate;
  result.seconds_95 = -std::log(1.0L - 0.95L) / rate;
  return result;
}

}  // namespace vanityforge

