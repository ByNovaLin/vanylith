#include "core/pattern.h"

#include "core/base58.h"

#include <algorithm>
#include <cmath>

namespace vanityforge {
namespace {

bool uses_prefix(const PatternPosition position) noexcept {
  return position == PatternPosition::prefix || position == PatternPosition::both;
}

bool uses_suffix(const PatternPosition position) noexcept {
  return position == PatternPosition::suffix || position == PatternPosition::both;
}

bool is_repeat(std::string_view value) noexcept {
  return !value.empty() && std::all_of(value.begin() + 1, value.end(),
                                       [first = value.front()](const char c) { return c == first; });
}

long double literal_probability(const std::size_t characters) {
  return std::pow(58.0L, -static_cast<long double>(characters));
}

long double repeat_probability(const std::size_t length) {
  // The first character can be any Base58 character. Each following character
  // must equal it, so P = 58 * (1/58)^length = 58^(1-length).
  return std::pow(58.0L, 1.0L - static_cast<long double>(length));
}

}  // namespace

PatternValidation validate_pattern(const PatternSpec& pattern) {
  constexpr std::size_t maximum_user_characters = 33;
  std::size_t prefix_length = 0;
  std::size_t suffix_length = 0;

  if (pattern.mode == PatternMode::literal) {
    if (uses_prefix(pattern.position)) {
      if (pattern.literal_prefix.empty()) {
        return {false, "Literal prefix must not be empty"};
      }
      if (!std::all_of(pattern.literal_prefix.begin(), pattern.literal_prefix.end(),
                       is_base58_character)) {
        return {false, "Literal prefix contains a character outside the Base58 alphabet"};
      }
      prefix_length = pattern.literal_prefix.size();
    }
    if (uses_suffix(pattern.position)) {
      if (pattern.literal_suffix.empty()) {
        return {false, "Literal suffix must not be empty"};
      }
      if (!std::all_of(pattern.literal_suffix.begin(), pattern.literal_suffix.end(),
                       is_base58_character)) {
        return {false, "Literal suffix contains a character outside the Base58 alphabet"};
      }
      suffix_length = pattern.literal_suffix.size();
    }
  } else {
    if (uses_prefix(pattern.position)) {
      if (pattern.repeat_prefix_length < 2 || pattern.repeat_prefix_length > 33) {
        return {false, "Repeat prefix length must be between 2 and 33"};
      }
      prefix_length = pattern.repeat_prefix_length;
    }
    if (uses_suffix(pattern.position)) {
      if (pattern.repeat_suffix_length < 2 || pattern.repeat_suffix_length > 33) {
        return {false, "Repeat suffix length must be between 2 and 33"};
      }
      suffix_length = pattern.repeat_suffix_length;
    }
  }

  if (prefix_length + suffix_length > maximum_user_characters) {
    return {false, "Combined prefix and suffix exceed the TRON address payload length"};
  }
  return {true, {}};
}

bool matches_pattern(const std::string_view tron_address, const PatternSpec& pattern) {
  if (tron_address.empty() || tron_address.front() != 'T' || !validate_pattern(pattern).valid) {
    return false;
  }

  const std::string_view user_prefix = tron_address.substr(1);
  if (pattern.mode == PatternMode::literal) {
    if (uses_prefix(pattern.position) && !user_prefix.starts_with(pattern.literal_prefix)) {
      return false;
    }
    if (uses_suffix(pattern.position) && !tron_address.ends_with(pattern.literal_suffix)) {
      return false;
    }
    return true;
  }

  if (uses_prefix(pattern.position)) {
    if (user_prefix.size() < pattern.repeat_prefix_length ||
        !is_repeat(user_prefix.substr(0, pattern.repeat_prefix_length))) {
      return false;
    }
  }
  if (uses_suffix(pattern.position)) {
    if (tron_address.size() < pattern.repeat_suffix_length ||
        !is_repeat(tron_address.substr(tron_address.size() - pattern.repeat_suffix_length))) {
      return false;
    }
  }
  return true;
}

long double match_probability(const PatternSpec& pattern) {
  const PatternValidation validation = validate_pattern(pattern);
  if (!validation.valid) {
    return 0.0L;
  }

  long double probability = 1.0L;
  if (pattern.mode == PatternMode::literal) {
    if (uses_prefix(pattern.position)) {
      probability *= literal_probability(pattern.literal_prefix.size());
    }
    if (uses_suffix(pattern.position)) {
      probability *= literal_probability(pattern.literal_suffix.size());
    }
  } else {
    if (uses_prefix(pattern.position)) {
      probability *= repeat_probability(pattern.repeat_prefix_length);
    }
    if (uses_suffix(pattern.position)) {
      probability *= repeat_probability(pattern.repeat_suffix_length);
    }
  }
  return probability;
}

}  // namespace vanityforge
