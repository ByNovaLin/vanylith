#include "core/keccak256.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>

namespace vanityforge {
namespace {

constexpr std::array<std::uint64_t, 24> round_constants = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

constexpr std::array<int, 24> rotation = {
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
    27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};

constexpr std::array<int, 24> permutation = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
    15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

std::uint64_t load_le64(const std::uint8_t* input) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(input[i]) << (i * 8);
  }
  return value;
}

void store_le64(std::uint8_t* output, const std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    output[i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
}

void keccak_f1600(std::array<std::uint64_t, 25>& state) noexcept {
  std::array<std::uint64_t, 5> column{};
  for (std::size_t round = 0; round < round_constants.size(); ++round) {
    for (std::size_t i = 0; i < 5; ++i) {
      column[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^ state[i + 15] ^ state[i + 20];
    }
    for (std::size_t i = 0; i < 5; ++i) {
      const std::uint64_t delta = column[(i + 4) % 5] ^ std::rotl(column[(i + 1) % 5], 1);
      for (std::size_t j = 0; j < 25; j += 5) {
        state[j + i] ^= delta;
      }
    }

    std::uint64_t current = state[1];
    for (std::size_t i = 0; i < permutation.size(); ++i) {
      const std::size_t destination = static_cast<std::size_t>(permutation[i]);
      const std::uint64_t saved = state[destination];
      state[destination] = std::rotl(current, rotation[i]);
      current = saved;
    }

    for (std::size_t row = 0; row < 25; row += 5) {
      const auto original = std::array<std::uint64_t, 5>{
          state[row], state[row + 1], state[row + 2], state[row + 3], state[row + 4]};
      for (std::size_t i = 0; i < 5; ++i) {
        state[row + i] = original[i] ^ ((~original[(i + 1) % 5]) & original[(i + 2) % 5]);
      }
    }
    state[0] ^= round_constants[round];
  }
}

}  // namespace

Hash256 keccak256(const std::span<const std::uint8_t> input) noexcept {
  constexpr std::size_t rate = 136;
  std::array<std::uint64_t, 25> state{};
  std::size_t consumed = 0;

  while (input.size() - consumed >= rate) {
    for (std::size_t lane = 0; lane < rate / 8; ++lane) {
      state[lane] ^= load_le64(input.data() + consumed + lane * 8);
    }
    keccak_f1600(state);
    consumed += rate;
  }

  std::array<std::uint8_t, rate> block{};
  const std::size_t remaining = input.size() - consumed;
  if (remaining != 0) {
    std::memcpy(block.data(), input.data() + consumed, remaining);
  }
  block[remaining] = 0x01;  // Keccak padding, deliberately not SHA3's 0x06.
  block[rate - 1] |= 0x80;
  for (std::size_t lane = 0; lane < rate / 8; ++lane) {
    state[lane] ^= load_le64(block.data() + lane * 8);
  }
  keccak_f1600(state);

  Hash256 output{};
  for (std::size_t lane = 0; lane < output.size() / 8; ++lane) {
    store_le64(output.data() + lane * 8, state[lane]);
  }
  return output;
}

}  // namespace vanityforge

