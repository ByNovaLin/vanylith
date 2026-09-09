#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vanityforge {

enum class PatternMode { literal, repeat };
enum class PatternPosition { prefix, suffix, both };

struct PatternSpec final {
  PatternMode mode = PatternMode::literal;
  PatternPosition position = PatternPosition::suffix;
  std::string literal_prefix;
  std::string literal_suffix;
  std::size_t repeat_prefix_length = 5;
  std::size_t repeat_suffix_length = 5;
};

struct PatternValidation final {
  bool valid = false;
  std::string error;
};

PatternValidation validate_pattern(const PatternSpec& pattern);
bool matches_pattern(std::string_view tron_address, const PatternSpec& pattern);
long double match_probability(const PatternSpec& pattern);

}  // namespace vanityforge
