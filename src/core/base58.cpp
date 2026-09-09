#include "core/base58.h"

#include <algorithm>
#include <vector>

namespace vanityforge {

bool is_base58_character(const char value) noexcept {
  return kBase58Alphabet.find(value) != std::string_view::npos;
}

std::string base58_encode(const std::span<const std::uint8_t> input) {
  std::size_t zeroes = 0;
  while (zeroes < input.size() && input[zeroes] == 0) {
    ++zeroes;
  }

  std::vector<std::uint8_t> digits((input.size() - zeroes) * 138 / 100 + 1);
  std::size_t length = 0;
  for (std::size_t i = zeroes; i < input.size(); ++i) {
    unsigned int carry = input[i];
    std::size_t used = 0;
    for (auto it = digits.rbegin();
         (carry != 0 || used < length) && it != digits.rend(); ++it, ++used) {
      carry += 256U * *it;
      *it = static_cast<std::uint8_t>(carry % 58U);
      carry /= 58U;
    }
    length = used;
  }

  auto first = digits.begin() + static_cast<std::ptrdiff_t>(digits.size() - length);
  while (first != digits.end() && *first == 0) {
    ++first;
  }

  std::string output(zeroes, '1');
  output.reserve(zeroes + static_cast<std::size_t>(digits.end() - first));
  while (first != digits.end()) {
    output.push_back(kBase58Alphabet[*first]);
    ++first;
  }
  return output;
}

}  // namespace vanityforge

