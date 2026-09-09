#include "backend/cuda/cuda_backend.h"

#include "backend/backend_registry.h"
#include "backend/cuda/cuda_test_api.h"
#include "core/secure_memory.h"

#include <cuda_runtime.h>
#include <secp256k1.h>

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vanityforge {
namespace {

#ifndef VANITYFORGE_CUDA_GROUP_THREADS
#define VANITYFORGE_CUDA_GROUP_THREADS 32
#endif
#ifndef VANITYFORGE_CUDA_CANDIDATES_PER_THREAD
#define VANITYFORGE_CUDA_CANDIDATES_PER_THREAD 64
#endif

constexpr std::uint32_t kThreadsPerBlock = VANITYFORGE_CUDA_GROUP_THREADS;
// A thread derives the first point in a short contiguous run with a full
// scalar multiplication, advances the remaining points with P + G, and uses
// one Montgomery batch inversion for the run. The three-stage pipeline keeps
// point, inversion, and address state in separate kernels so no thread owns
// the whole group at once. A 64-key group keeps 16,384 groups in flight.
constexpr std::uint32_t kCandidatesPerThread = VANITYFORGE_CUDA_CANDIDATES_PER_THREAD;
static_assert(kThreadsPerBlock == 32 || kThreadsPerBlock == 64);
static_assert(kCandidatesPerThread == 32 || kCandidatesPerThread == 64 ||
              kCandidatesPerThread == 128);
constexpr std::uint32_t kSearchBatchSize = 1048576;
constexpr std::uint32_t kHashThreadsPerBlock = 128;
constexpr std::uint8_t kPatternLiteral = 0;
constexpr std::uint8_t kPatternRepeat = 1;
constexpr std::uint8_t kPatternPrefix = 0;
constexpr std::uint8_t kPatternSuffix = 1;
constexpr std::uint8_t kPatternBoth = 2;

struct DeviceKey final {
  std::uint8_t bytes[32];
};

struct DevicePattern final {
  std::uint8_t mode = kPatternLiteral;
  std::uint8_t position = kPatternSuffix;
  std::uint8_t prefix_length = 0;
  std::uint8_t suffix_length = 0;
  char prefix[33]{};
  char suffix[33]{};
};

struct GpuMatch final {
  std::uint8_t private_key[32];
  char address[35];
};

struct Field final {
  // Little-endian base-2^32 words modulo secp256k1's field prime.
  std::uint32_t words[8];
};

struct Point final {
  Field x;
  Field y;
  Field z;
  bool infinity;
};

struct AffinePoint final {
  Field x;
  Field y;
};

// 64 base-16 positions, each with the nonzero multiples 1..15. Global
// read-only caching performs better than constant-memory serialization when
// lanes in a warp select different scalar digits.
__device__ AffinePoint generator_window[64][15];

__device__ inline std::uint32_t field_prime_word(const int index) {
  if (index == 0) return 0xfffffc2fU;
  if (index == 1) return 0xfffffffeU;
  return 0xffffffffU;
}

__device__ inline void field_zero(Field& value) {
  for (int index = 0; index < 8; ++index) value.words[index] = 0;
}

__device__ inline void field_one(Field& value) {
  field_zero(value);
  value.words[0] = 1;
}

__device__ inline bool field_is_zero(const Field& value) {
  std::uint32_t combined = 0;
  for (int index = 0; index < 8; ++index) combined |= value.words[index];
  return combined == 0;
}

__device__ inline bool field_is_at_least_prime(const Field& value) {
  for (int index = 7; index >= 0; --index) {
    const std::uint32_t prime = field_prime_word(index);
    if (value.words[index] > prime) return true;
    if (value.words[index] < prime) return false;
  }
  return true;
}

__device__ inline void field_reduce_once(Field& value) {
  if (!field_is_at_least_prime(value)) return;
  Field reduced{};
  std::uint64_t borrow = 0;
  for (int index = 0; index < 8; ++index) {
    const std::uint64_t subtrahend = static_cast<std::uint64_t>(field_prime_word(index)) + borrow;
    reduced.words[index] = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(value.words[index]) - subtrahend);
    borrow = static_cast<std::uint64_t>(value.words[index]) < subtrahend ? 1U : 0U;
  }
  value = reduced;
}

// secp256k1 p is 2^256 - (2^32 + 977). Fold one overflow word back into
// the field. A second fold is only needed for the extremely rare carry-out
// from this fold itself.
__device__ inline void field_add_overflow_constant(Field& value) {
  for (;;) {
    std::uint64_t sum = static_cast<std::uint64_t>(value.words[0]) + 977U;
    value.words[0] = static_cast<std::uint32_t>(sum);
    std::uint64_t carry = sum >> 32U;
    sum = static_cast<std::uint64_t>(value.words[1]) + 1U + carry;
    value.words[1] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
    for (int index = 2; index < 8; ++index) {
      sum = static_cast<std::uint64_t>(value.words[index]) + carry;
      value.words[index] = static_cast<std::uint32_t>(sum);
      carry = sum >> 32U;
    }
    if (carry == 0) return;
  }
}

__device__ inline void field_add(const Field& left, const Field& right, Field& output) {
  Field result{};
  std::uint64_t carry = 0;
  for (int index = 0; index < 8; ++index) {
    const std::uint64_t sum = static_cast<std::uint64_t>(left.words[index]) +
                              static_cast<std::uint64_t>(right.words[index]) + carry;
    result.words[index] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
  }
  if (carry != 0) field_add_overflow_constant(result);
  field_reduce_once(result);
  field_reduce_once(result);
  output = result;
}

__device__ inline void field_subtract(const Field& left, const Field& right, Field& output) {
  Field result{};
  std::uint64_t borrow = 0;
  for (int index = 0; index < 8; ++index) {
    const std::uint64_t subtrahend = static_cast<std::uint64_t>(right.words[index]) + borrow;
    result.words[index] = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(left.words[index]) - subtrahend);
    borrow = static_cast<std::uint64_t>(left.words[index]) < subtrahend ? 1U : 0U;
  }
  if (borrow != 0) {
    std::uint64_t carry = 0;
    for (int index = 0; index < 8; ++index) {
      const std::uint64_t sum = static_cast<std::uint64_t>(result.words[index]) +
                                static_cast<std::uint64_t>(field_prime_word(index)) + carry;
      result.words[index] = static_cast<std::uint32_t>(sum);
      carry = sum >> 32U;
    }
  }
  output = result;
}

// Column-wise multiplication keeps the accumulator's overflow separate from
// its low 64 bits. This avoids both the carry loss possible when a 64-bit
// product and two carry words are added directly and the dynamic propagation
// loops that compile especially poorly on Ampere.
__device__ inline void field_multiply(const Field& left, const Field& right, Field& output) {
  std::uint32_t limbs[16]{};
  std::uint64_t carry = 0;
#pragma unroll
  for (int column = 0; column < 16; ++column) {
    std::uint64_t accumulator = carry;
    std::uint32_t overflow = 0;
#pragma unroll
    for (int left_index = 0; left_index < 8; ++left_index) {
      const int right_index = column - left_index;
      if (right_index >= 0 && right_index < 8) {
        const std::uint64_t previous = accumulator;
        const std::uint64_t product = static_cast<std::uint64_t>(left.words[left_index]) *
                                      static_cast<std::uint64_t>(right.words[right_index]);
        accumulator += product;
        overflow += accumulator < previous ? 1U : 0U;
      }
    }
    limbs[column] = static_cast<std::uint32_t>(accumulator);
    carry = (static_cast<std::uint64_t>(overflow) << 32U) | (accumulator >> 32U);
  }

  // Fold the upper 256 bits with B^8 == B + 977 (mod p), B = 2^32.
  // Each column sum is below 2^43, so it is exact in a 64-bit word.
  Field result{};
  carry = 0;
#pragma unroll
  for (int index = 0; index < 8; ++index) {
    std::uint64_t sum = static_cast<std::uint64_t>(limbs[index]) + carry +
                        static_cast<std::uint64_t>(limbs[index + 8]) * 977U;
    if (index != 0) sum += limbs[index + 7];
    result.words[index] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
  }

  // The top high limb contributes at B^8, as does the carry from the first
  // fold. Fold that bounded value once more, then consume a possible final
  // single-word overflow with the existing bounded field helper.
  const std::uint64_t overflow = carry + limbs[15];
  std::uint64_t sum = static_cast<std::uint64_t>(result.words[0]) + overflow * 977U;
  result.words[0] = static_cast<std::uint32_t>(sum);
  carry = sum >> 32U;
  sum = static_cast<std::uint64_t>(result.words[1]) + overflow + carry;
  result.words[1] = static_cast<std::uint32_t>(sum);
  carry = sum >> 32U;
#pragma unroll
  for (int index = 2; index < 8; ++index) {
    sum = static_cast<std::uint64_t>(result.words[index]) + carry;
    result.words[index] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
  }
  if (carry != 0) field_add_overflow_constant(result);
  field_reduce_once(result);
  field_reduce_once(result);
  output = result;
}

__device__ inline void field_square(const Field& value, Field& output) {
  field_multiply(value, value, output);
}

__device__ inline void field_square_n(const Field& value, const int count, Field& output) {
  output = value;
  for (int index = 0; index < count; ++index) field_square(output, output);
}

__device__ inline void field_inverse(const Field& value, Field& output) {
  // Fixed addition chain for p - 2 = 2^256 - 2^32 - 979. This retains the
  // exact Fermat inverse but needs 255 squarings and 15 multiplies instead of
  // the binary exponentiation loop's 255 squarings and 249 multiplies.
  Field x2{};
  field_square(value, x2);
  field_multiply(x2, value, x2);                 // 2^2 - 1
  Field x3{};
  field_square(x2, x3);
  field_multiply(x3, value, x3);                 // 2^3 - 1
  Field x6{};
  field_square_n(x3, 3, x6);
  field_multiply(x6, x3, x6);                    // 2^6 - 1
  Field x9{};
  field_square_n(x6, 3, x9);
  field_multiply(x9, x3, x9);                    // 2^9 - 1
  Field x11{};
  field_square_n(x9, 2, x11);
  field_multiply(x11, x2, x11);                  // 2^11 - 1
  Field x22{};
  field_square_n(x11, 11, x22);
  field_multiply(x22, x11, x22);                 // 2^22 - 1
  Field x44{};
  field_square_n(x22, 22, x44);
  field_multiply(x44, x22, x44);                 // 2^44 - 1
  Field x88{};
  field_square_n(x44, 44, x88);
  field_multiply(x88, x44, x88);                 // 2^88 - 1
  Field x176{};
  field_square_n(x88, 88, x176);
  field_multiply(x176, x88, x176);               // 2^176 - 1
  Field x220{};
  field_square_n(x176, 44, x220);
  field_multiply(x220, x44, x220);               // 2^220 - 1
  Field x223{};
  field_square_n(x220, 3, x223);
  field_multiply(x223, x3, x223);                 // 2^223 - 1

  Field result{};
  field_square_n(x223, 23, result);
  field_multiply(result, x22, result);
  field_square_n(result, 5, result);
  field_multiply(result, value, result);
  field_square_n(result, 3, result);
  field_multiply(result, x2, result);
  field_square_n(result, 2, result);
  field_multiply(result, value, output);
}

__device__ inline void point_set_infinity(Point& point) {
  field_zero(point.x);
  field_zero(point.y);
  field_zero(point.z);
  point.infinity = true;
}

__device__ inline void point_set_generator(Point& point) {
  // G.x = 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798
  point.x.words[0] = 0x16f81798U;
  point.x.words[1] = 0x59f2815bU;
  point.x.words[2] = 0x2dce28d9U;
  point.x.words[3] = 0x029bfcdbU;
  point.x.words[4] = 0xce870b07U;
  point.x.words[5] = 0x55a06295U;
  point.x.words[6] = 0xf9dcbbacU;
  point.x.words[7] = 0x79be667eU;
  // G.y = 483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8
  point.y.words[0] = 0xfb10d4b8U;
  point.y.words[1] = 0x9c47d08fU;
  point.y.words[2] = 0xa6855419U;
  point.y.words[3] = 0xfd17b448U;
  point.y.words[4] = 0x0e1108a8U;
  point.y.words[5] = 0x5da4fbfcU;
  point.y.words[6] = 0x26a3c465U;
  point.y.words[7] = 0x483ada77U;
  field_one(point.z);
  point.infinity = false;
}

__device__ inline void point_double(Point& point) {
  if (point.infinity || field_is_zero(point.y)) {
    point_set_infinity(point);
    return;
  }

  Field a{};
  Field b{};
  Field c{};
  Field d{};
  Field e{};
  Field f{};
  Field temporary{};
  Field x3{};
  Field y3{};
  Field z3{};

  field_square(point.x, a);
  field_square(point.y, b);
  field_square(b, c);
  field_add(point.x, b, temporary);
  field_square(temporary, temporary);
  field_subtract(temporary, a, temporary);
  field_subtract(temporary, c, temporary);
  field_add(temporary, temporary, d);
  field_add(a, a, e);
  field_add(e, a, e);
  field_square(e, f);
  field_add(d, d, temporary);
  field_subtract(f, temporary, x3);
  field_subtract(d, x3, temporary);
  field_multiply(e, temporary, y3);
  field_add(c, c, temporary);
  field_add(temporary, temporary, temporary);
  field_add(temporary, temporary, temporary);
  field_subtract(y3, temporary, y3);
  field_multiply(point.y, point.z, z3);
  field_add(z3, z3, z3);

  point.x = x3;
  point.y = y3;
  point.z = z3;
  point.infinity = false;
}

__device__ inline void point_add_affine(Point& point, const AffinePoint& affine) {
  if (point.infinity) {
    point.x = affine.x;
    point.y = affine.y;
    field_one(point.z);
    point.infinity = false;
    return;
  }

  Field z_squared{};
  Field u2{};
  Field z_cubed{};
  Field s2{};
  Field h{};
  Field r{};
  field_square(point.z, z_squared);
  field_multiply(affine.x, z_squared, u2);
  field_multiply(z_squared, point.z, z_cubed);
  field_multiply(affine.y, z_cubed, s2);
  field_subtract(u2, point.x, h);
  field_subtract(s2, point.y, r);
  if (field_is_zero(h)) {
    if (field_is_zero(r)) point_double(point);
    else point_set_infinity(point);
    return;
  }

  Field hh{};
  Field i{};
  Field j{};
  Field r_twice{};
  Field v{};
  Field x3{};
  Field y3{};
  Field z3{};
  Field temporary{};

  field_square(h, hh);
  field_add(hh, hh, i);
  field_add(i, i, i);
  field_multiply(h, i, j);
  field_add(r, r, r_twice);
  field_multiply(point.x, i, v);
  field_square(r_twice, x3);
  field_subtract(x3, j, x3);
  field_add(v, v, temporary);
  field_subtract(x3, temporary, x3);
  field_subtract(v, x3, temporary);
  field_multiply(r_twice, temporary, y3);
  field_multiply(point.y, j, temporary);
  field_add(temporary, temporary, temporary);
  field_subtract(y3, temporary, y3);
  field_add(point.z, h, z3);
  field_square(z3, z3);
  field_subtract(z3, z_squared, z3);
  field_subtract(z3, hh, z3);

  point.x = x3;
  point.y = y3;
  point.z = z3;
  point.infinity = false;
}

__device__ inline void point_add_generator(Point& point) {
  Point generator{};
  point_set_generator(generator);
  const AffinePoint affine{generator.x, generator.y};
  point_add_affine(point, affine);
}

__device__ inline void field_to_big_endian(const Field& value, std::uint8_t output[32]) {
  for (int word = 0; word < 8; ++word) {
    const std::uint32_t value_word = value.words[7 - word];
    output[word * 4] = static_cast<std::uint8_t>(value_word >> 24U);
    output[word * 4 + 1] = static_cast<std::uint8_t>(value_word >> 16U);
    output[word * 4 + 2] = static_cast<std::uint8_t>(value_word >> 8U);
    output[word * 4 + 3] = static_cast<std::uint8_t>(value_word);
  }
}

__device__ inline bool scalar_is_valid(const std::uint8_t scalar[32]) {
  const std::uint8_t order[32] = {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
      0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
      0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};
  bool nonzero = false;
  for (int index = 0; index < 32; ++index) nonzero = nonzero || scalar[index] != 0;
  if (!nonzero) return false;
  for (int index = 0; index < 32; ++index) {
    if (scalar[index] < order[index]) return true;
    if (scalar[index] > order[index]) return false;
  }
  return false;
}

__device__ inline bool scalar_at_offset(const DeviceKey base_key, const std::uint64_t offset,
                                         std::uint8_t scalar[32]) {
  for (int index = 0; index < 32; ++index) scalar[index] = base_key.bytes[index];
  std::uint64_t carry = offset;
  for (int index = 31; index >= 0 && carry != 0; --index) {
    const std::uint64_t sum = static_cast<std::uint64_t>(scalar[index]) + (carry & 0xffU);
    scalar[index] = static_cast<std::uint8_t>(sum);
    carry = (carry >> 8U) + (sum >> 8U);
  }
  return carry == 0 && scalar_is_valid(scalar);
}

__device__ inline bool scalar_multiply_generator_jacobian(const std::uint8_t scalar[32],
                                                           Point& point) {
  point_set_infinity(point);
  for (int window = 0; window < 64; ++window) {
    const std::uint8_t byte = scalar[window / 2];
    const std::uint8_t digit = static_cast<std::uint8_t>(
        window % 2 == 0 ? byte >> 4U : byte & 0x0fU);
    if (digit != 0) point_add_affine(point, generator_window[window][digit - 1U]);
  }
  return !point.infinity && !field_is_zero(point.z);
}

__device__ inline void point_to_public_xy(const Point& point, const Field& z_inverse,
                                          std::uint8_t public_xy[64]) {
  Field z_inverse_squared{};
  Field z_inverse_cubed{};
  Field x{};
  Field y{};
  field_square(z_inverse, z_inverse_squared);
  field_multiply(z_inverse_squared, z_inverse, z_inverse_cubed);
  field_multiply(point.x, z_inverse_squared, x);
  field_multiply(point.y, z_inverse_cubed, y);
  field_to_big_endian(x, public_xy);
  field_to_big_endian(y, public_xy + 32);
}

__device__ inline std::uint64_t rotate_left_64(const std::uint64_t value, const int amount) {
  return (value << amount) | (value >> (64 - amount));
}

__device__ inline void keccak_f1600(std::uint64_t state[25]) {
  const std::uint64_t round_constants[24] = {
      0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
      0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
      0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
      0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
      0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
      0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
      0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
      0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
  const int rotation[24] = {
      1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
      27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
  const int permutation[24] = {
      10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
      15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};
  for (int round = 0; round < 24; ++round) {
    std::uint64_t column[5]{};
    for (int index = 0; index < 5; ++index) {
      column[index] = state[index] ^ state[index + 5] ^ state[index + 10] ^
                      state[index + 15] ^ state[index + 20];
    }
    for (int index = 0; index < 5; ++index) {
      const std::uint64_t delta = column[(index + 4) % 5] ^
                                  rotate_left_64(column[(index + 1) % 5], 1);
      for (int row = 0; row < 25; row += 5) state[row + index] ^= delta;
    }
    std::uint64_t current = state[1];
    for (int index = 0; index < 24; ++index) {
      const int destination = permutation[index];
      const std::uint64_t saved = state[destination];
      state[destination] = rotate_left_64(current, rotation[index]);
      current = saved;
    }
    for (int row = 0; row < 25; row += 5) {
      const std::uint64_t original[5] = {
          state[row], state[row + 1], state[row + 2], state[row + 3], state[row + 4]};
      for (int index = 0; index < 5; ++index) {
        state[row + index] = original[index] ^
                             ((~original[(index + 1) % 5]) & original[(index + 2) % 5]);
      }
    }
    state[0] ^= round_constants[round];
  }
}

__device__ inline void keccak256_64(const std::uint8_t input[64], std::uint8_t output[32]) {
  std::uint64_t state[25] = {};
  for (int index = 0; index < 64; ++index) {
    state[index / 8] ^= static_cast<std::uint64_t>(input[index]) << ((index % 8) * 8);
  }
  // Legacy Keccak padding, deliberately not SHA3's 0x06 domain byte.
  state[8] ^= 0x01ULL;
  state[16] ^= 0x8000000000000000ULL;
  keccak_f1600(state);
  for (int lane = 0; lane < 4; ++lane) {
    for (int byte = 0; byte < 8; ++byte) {
      output[lane * 8 + byte] = static_cast<std::uint8_t>(state[lane] >> (byte * 8));
    }
  }
}

__device__ inline std::uint32_t rotate_right_32(const std::uint32_t value, const int amount) {
  return (value >> amount) | (value << (32 - amount));
}

__device__ inline void sha256_small(const std::uint8_t* input, const int input_size,
                                    std::uint8_t output[32]) {
  const std::uint32_t constants[64] = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
      0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
      0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
      0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
      0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
  std::uint8_t block[64] = {};
  for (int index = 0; index < input_size; ++index) block[index] = input[index];
  block[input_size] = 0x80U;
  const std::uint64_t bit_count = static_cast<std::uint64_t>(input_size) * 8U;
  for (int index = 0; index < 8; ++index) {
    block[63 - index] = static_cast<std::uint8_t>(bit_count >> (index * 8));
  }

  std::uint32_t words[64]{};
  for (int index = 0; index < 16; ++index) {
    words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24U) |
                   (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U) |
                   static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (int index = 16; index < 64; ++index) {
    const std::uint32_t sigma0 = rotate_right_32(words[index - 15], 7) ^
                                 rotate_right_32(words[index - 15], 18) ^ (words[index - 15] >> 3U);
    const std::uint32_t sigma1 = rotate_right_32(words[index - 2], 17) ^
                                 rotate_right_32(words[index - 2], 19) ^ (words[index - 2] >> 10U);
    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
  }

  std::uint32_t a = 0x6a09e667U;
  std::uint32_t b = 0xbb67ae85U;
  std::uint32_t c = 0x3c6ef372U;
  std::uint32_t d = 0xa54ff53aU;
  std::uint32_t e = 0x510e527fU;
  std::uint32_t f = 0x9b05688cU;
  std::uint32_t g = 0x1f83d9abU;
  std::uint32_t h = 0x5be0cd19U;
  for (int index = 0; index < 64; ++index) {
    const std::uint32_t sigma1 = rotate_right_32(e, 6) ^ rotate_right_32(e, 11) ^
                                 rotate_right_32(e, 25);
    const std::uint32_t choice = (e & f) ^ ((~e) & g);
    const std::uint32_t temporary1 = h + sigma1 + choice + constants[index] + words[index];
    const std::uint32_t sigma0 = rotate_right_32(a, 2) ^ rotate_right_32(a, 13) ^
                                 rotate_right_32(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  const std::uint32_t digest[8] = {
      a + 0x6a09e667U, b + 0xbb67ae85U, c + 0x3c6ef372U, d + 0xa54ff53aU,
      e + 0x510e527fU, f + 0x9b05688cU, g + 0x1f83d9abU, h + 0x5be0cd19U};
  for (int index = 0; index < 8; ++index) {
    output[index * 4] = static_cast<std::uint8_t>(digest[index] >> 24U);
    output[index * 4 + 1] = static_cast<std::uint8_t>(digest[index] >> 16U);
    output[index * 4 + 2] = static_cast<std::uint8_t>(digest[index] >> 8U);
    output[index * 4 + 3] = static_cast<std::uint8_t>(digest[index]);
  }
}

__device__ inline int base58_encode_25(const std::uint8_t input[25], char output[35]) {
  const char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  constexpr std::uint32_t chunk_base = 656356768U;  // 58^5
  // Treat the 25-byte number as seven big-endian 32-bit words. Dividing by
  // 58^5 emits five Base58 digits per pass and avoids the byte-by-byte
  // repeated division used by the correctness-first encoder.
  std::uint32_t limbs[7]{};
  limbs[0] = input[0];
  for (int word = 1; word < 7; ++word) {
    const int offset = 1 + (word - 1) * 4;
    limbs[word] = (static_cast<std::uint32_t>(input[offset]) << 24U) |
                  (static_cast<std::uint32_t>(input[offset + 1]) << 16U) |
                  (static_cast<std::uint32_t>(input[offset + 2]) << 8U) |
                  static_cast<std::uint32_t>(input[offset + 3]);
  }

  char reversed[35]{};
  int length = 0;
  for (;;) {
    std::uint64_t remainder = 0;
    bool quotient_nonzero = false;
    for (int word = 0; word < 7; ++word) {
      const std::uint64_t dividend = (remainder << 32U) | limbs[word];
      const std::uint32_t quotient = static_cast<std::uint32_t>(dividend / chunk_base);
      remainder = dividend - static_cast<std::uint64_t>(quotient) * chunk_base;
      limbs[word] = quotient;
      quotient_nonzero = quotient_nonzero || quotient != 0;
    }
    const int digits_to_emit = quotient_nonzero ? 5 : 0;
    for (int digit = 0; digit < digits_to_emit; ++digit) {
      if (length >= 35) return 0;
      reversed[length++] = alphabet[remainder % 58U];
      remainder /= 58U;
    }
    if (!quotient_nonzero) {
      while (remainder != 0) {
        if (length >= 35) return 0;
        reversed[length++] = alphabet[remainder % 58U];
        remainder /= 58U;
      }
      break;
    }
  }
  if (length <= 0 || length >= 35) return 0;
  for (int index = 0; index < length; ++index) output[index] = reversed[length - 1 - index];
  output[length] = '\0';
  return length;
}

__device__ inline bool device_matches_pattern(const char address[35], const int address_length,
                                              const DevicePattern pattern) {
  if (address_length <= 1 || address[0] != 'T') return false;
  const bool use_prefix = pattern.position == kPatternPrefix || pattern.position == kPatternBoth;
  const bool use_suffix = pattern.position == kPatternSuffix || pattern.position == kPatternBoth;
  if (pattern.mode == kPatternLiteral) {
    if (use_prefix) {
      if (1 + pattern.prefix_length > address_length) return false;
      for (int index = 0; index < pattern.prefix_length; ++index) {
        if (address[1 + index] != pattern.prefix[index]) return false;
      }
    }
    if (use_suffix) {
      if (pattern.suffix_length > address_length) return false;
      const int start = address_length - pattern.suffix_length;
      for (int index = 0; index < pattern.suffix_length; ++index) {
        if (address[start + index] != pattern.suffix[index]) return false;
      }
    }
    return true;
  }
  if (pattern.mode != kPatternRepeat) return false;
  if (use_prefix) {
    if (1 + pattern.prefix_length > address_length || pattern.prefix_length < 2) return false;
    const char repeated = address[1];
    for (int index = 1; index < pattern.prefix_length; ++index) {
      if (address[1 + index] != repeated) return false;
    }
  }
  if (use_suffix) {
    if (pattern.suffix_length > address_length || pattern.suffix_length < 2) return false;
    const int start = address_length - pattern.suffix_length;
    const char repeated = address[start];
    for (int index = 1; index < pattern.suffix_length; ++index) {
      if (address[start + index] != repeated) return false;
    }
  }
  return true;
}

__global__ void point_progression_kernel(const DeviceKey base_key,
                                         const std::uint64_t begin_offset,
                                         const std::uint32_t candidate_count,
                                         const std::uint32_t workspace_stride,
                                         Point* points, std::uint32_t* group_counts,
                                         std::uint32_t* pipeline_failure_count) {
  const std::uint64_t group_index =
      static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const std::uint64_t first_candidate = group_index * kCandidatesPerThread;
  if (first_candidate >= candidate_count || first_candidate > ~std::uint64_t{0} - begin_offset) return;

  std::uint8_t scalar[32]{};
  if (!scalar_at_offset(base_key, begin_offset + first_candidate, scalar)) return;

  Point current{};
  if (!scalar_multiply_generator_jacobian(scalar, current)) {
    atomicAdd(pipeline_failure_count, 1U);
    return;
  }

  std::uint32_t group_count =
      min(kCandidatesPerThread, candidate_count - static_cast<std::uint32_t>(first_candidate));
  const std::uint64_t maximum_candidate = ~std::uint64_t{0} - begin_offset;
  if (static_cast<std::uint64_t>(group_count - 1U) > maximum_candidate - first_candidate) {
    group_count = static_cast<std::uint32_t>(maximum_candidate - first_candidate + 1U);
  }
  const std::uint64_t last_candidate = first_candidate + group_count - 1U;
  if (!scalar_at_offset(base_key, begin_offset + last_candidate, scalar)) {
    group_count = 1;
    while (group_count < kCandidatesPerThread &&
           first_candidate + group_count < candidate_count &&
           first_candidate + group_count <= maximum_candidate &&
           scalar_at_offset(base_key, begin_offset + first_candidate + group_count, scalar)) {
      ++group_count;
    }
  }

  // Position-major storage makes adjacent group threads in a warp write the
  // same point position contiguously instead of striding by a whole group.
  for (std::uint32_t index = 0; index < group_count; ++index) {
    const std::uint64_t workspace_index =
        static_cast<std::uint64_t>(index) * workspace_stride + group_index;
    points[workspace_index] = current;
    if (index + 1U < group_count) {
      point_add_generator(current);
      if (current.infinity || field_is_zero(current.z)) {
        group_count = index + 1U;
        break;
      }
    }
  }
  group_counts[group_index] = group_count;
}

__global__ void batch_inversion_kernel(const std::uint32_t candidate_count,
                                       const std::uint32_t workspace_stride,
                                       const Point* points,
                                       const std::uint32_t* group_counts,
                                       Field* prefix_products, Field* z_inverses) {
  const std::uint64_t group_index =
      static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const std::uint64_t first_candidate = group_index * kCandidatesPerThread;
  if (first_candidate >= candidate_count) return;
  const std::uint32_t group_count = group_counts[group_index];
  if (group_count == 0) return;

  Field product{};
  field_one(product);
  for (std::uint32_t index = 0; index < group_count; ++index) {
    const std::uint64_t workspace_index =
        static_cast<std::uint64_t>(index) * workspace_stride + group_index;
    Field next_product{};
    field_multiply(product, points[workspace_index].z, next_product);
    product = next_product;
    prefix_products[workspace_index] = product;
  }

  Field inverse_product{};
  field_inverse(product, inverse_product);
  for (int index = static_cast<int>(group_count) - 1; index >= 0; --index) {
    const std::uint64_t workspace_index =
        static_cast<std::uint64_t>(index) * workspace_stride + group_index;
    Field z_inverse{};
    if (index == 0) {
      z_inverse = inverse_product;
    } else {
      field_multiply(inverse_product, prefix_products[workspace_index - workspace_stride], z_inverse);
    }
    Field next_inverse_product{};
    field_multiply(inverse_product, points[workspace_index].z, next_inverse_product);
    inverse_product = next_inverse_product;
    z_inverses[workspace_index] = z_inverse;
  }
}

__global__ void address_match_kernel(const DeviceKey base_key,
                                     const std::uint64_t begin_offset,
                                     const std::uint32_t candidate_count,
                                     const std::uint32_t workspace_stride,
                                     const DevicePattern pattern, const bool collect_all,
                                     const Point* points, const Field* z_inverses,
                                     const std::uint32_t* group_counts,
                                     GpuMatch* matches, std::uint32_t* match_count,
                                     std::uint32_t* valid_count,
                                     std::uint32_t* pipeline_failure_count,
                                     const std::uint32_t match_capacity) {
  const std::uint64_t workspace_index =
      static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  bool successful = false;
  const std::uint64_t workspace_count =
      static_cast<std::uint64_t>(workspace_stride) * kCandidatesPerThread;
  if (workspace_index < workspace_count) {
    // Mirror the position-major layout so the address stage also reads
    // adjacent point and inverse values across a warp.
    const std::uint32_t index_in_group =
        static_cast<std::uint32_t>(workspace_index / workspace_stride);
    const std::uint32_t group_index =
        static_cast<std::uint32_t>(workspace_index % workspace_stride);
    const std::uint64_t candidate_index =
        static_cast<std::uint64_t>(group_index) * kCandidatesPerThread + index_in_group;
    if (candidate_index < candidate_count && index_in_group < group_counts[group_index]) {
      std::uint8_t scalar[32]{};
      if (!scalar_at_offset(base_key, begin_offset + candidate_index, scalar)) {
        atomicAdd(pipeline_failure_count, 1U);
      } else {
        std::uint8_t public_xy[64]{};
        point_to_public_xy(points[workspace_index], z_inverses[workspace_index], public_xy);
        std::uint8_t keccak[32]{};
        keccak256_64(public_xy, keccak);
        std::uint8_t raw_address[21]{};
        raw_address[0] = 0x41U;
        for (int byte = 0; byte < 20; ++byte) raw_address[byte + 1] = keccak[byte + 12];
        std::uint8_t first_checksum[32]{};
        std::uint8_t second_checksum[32]{};
        sha256_small(raw_address, 21, first_checksum);
        sha256_small(first_checksum, 32, second_checksum);
        std::uint8_t checked_address[25]{};
        for (int byte = 0; byte < 21; ++byte) checked_address[byte] = raw_address[byte];
        for (int byte = 0; byte < 4; ++byte) checked_address[byte + 21] = second_checksum[byte];
        char encoded_address[35]{};
        const int address_length = base58_encode_25(checked_address, encoded_address);
        if (address_length == 0) {
          atomicAdd(pipeline_failure_count, 1U);
        } else {
          successful = true;
          if (collect_all || device_matches_pattern(encoded_address, address_length, pattern)) {
            const std::uint32_t match_index = atomicAdd(match_count, 1U);
            if (match_index < match_capacity) {
              GpuMatch result{};
              for (int byte = 0; byte < 32; ++byte) result.private_key[byte] = scalar[byte];
              for (int byte = 0; byte < 35; ++byte) result.address[byte] = encoded_address[byte];
              matches[match_index] = result;
            }
          }
        }
      }
    }
  }
  const int block_successful = __syncthreads_count(successful);
  if (threadIdx.x == 0 && block_successful != 0) {
    atomicAdd(valid_count, static_cast<std::uint32_t>(block_successful));
  }
}

std::string cuda_error_message(const char* operation, const cudaError_t status) {
  const char* detail = cudaGetErrorString(status);
  return std::string(operation) + ": " + (detail == nullptr ? "unknown CUDA error" : detail);
}

void throw_if_cuda_error(const cudaError_t status, const char* operation) {
  if (status != cudaSuccess) throw std::runtime_error(cuda_error_message(operation, status));
}

Field host_field_from_big_endian(const std::uint8_t* bytes) {
  Field field{};
  for (int word = 0; word < 8; ++word) {
    field.words[7 - word] =
        (static_cast<std::uint32_t>(bytes[word * 4]) << 24U) |
        (static_cast<std::uint32_t>(bytes[word * 4 + 1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[word * 4 + 2]) << 8U) |
        static_cast<std::uint32_t>(bytes[word * 4 + 3]);
  }
  return field;
}

void ensure_generator_window(const int cuda_ordinal) {
  static std::mutex mutex;
  static std::set<int> initialized_devices;
  std::lock_guard lock(mutex);
  if (initialized_devices.contains(cuda_ordinal)) return;

  std::array<AffinePoint, 64 * 15> table{};
  secp256k1_context* context = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  if (context == nullptr) throw std::runtime_error("Unable to create fixed-base table context");
  try {
    for (int window = 0; window < 64; ++window) {
      for (int digit = 1; digit <= 15; ++digit) {
        std::array<std::uint8_t, 32> scalar{};
        if (window % 2 == 0) {
          scalar[window / 2] = static_cast<std::uint8_t>(digit << 4);
        } else {
          scalar[window / 2] = static_cast<std::uint8_t>(digit);
        }
        secp256k1_pubkey public_key{};
        if (secp256k1_ec_pubkey_create(context, &public_key, scalar.data()) != 1) {
          throw std::runtime_error("Unable to derive fixed-base table point");
        }
        std::array<std::uint8_t, 65> serialized{};
        std::size_t serialized_size = serialized.size();
        if (secp256k1_ec_pubkey_serialize(context, serialized.data(), &serialized_size,
                                          &public_key, SECP256K1_EC_UNCOMPRESSED) != 1 ||
            serialized_size != serialized.size() || serialized[0] != 0x04U) {
          throw std::runtime_error("Unable to serialize fixed-base table point");
        }
        AffinePoint& entry = table[static_cast<std::size_t>(window) * 15U + digit - 1U];
        entry.x = host_field_from_big_endian(serialized.data() + 1);
        entry.y = host_field_from_big_endian(serialized.data() + 33);
        secure_zero(scalar.data(), scalar.size());
        secure_zero(serialized.data(), serialized.size());
      }
    }
    secp256k1_context_destroy(context);
    context = nullptr;
    throw_if_cuda_error(cudaSetDevice(cuda_ordinal), "cudaSetDevice(fixed-base table)");
    throw_if_cuda_error(
        cudaMemcpyToSymbol(generator_window, table.data(), sizeof(table), 0, cudaMemcpyHostToDevice),
        "cudaMemcpyToSymbol(fixed-base table)");
    secure_zero(table.data(), sizeof(table));
    initialized_devices.insert(cuda_ordinal);
  } catch (...) {
    if (context != nullptr) secp256k1_context_destroy(context);
    secure_zero(table.data(), sizeof(table));
    throw;
  }
}

struct BatchOutcome final {
  std::uint32_t valid_count = 0;
  std::vector<GpuMatch> matches;
};

void secure_zero_matches(std::vector<GpuMatch>& matches) noexcept {
  if (!matches.empty()) secure_zero(matches.data(), matches.size() * sizeof(GpuMatch));
  matches.clear();
}

class DeviceBuffers final {
 public:
  DeviceBuffers(const int cuda_ordinal, const std::uint32_t capacity)
      : cuda_ordinal_(cuda_ordinal), capacity_(capacity) {
    if (capacity_ == 0) throw std::invalid_argument("CUDA batch capacity must be nonzero");
    throw_if_cuda_error(cudaSetDevice(cuda_ordinal_), "cudaSetDevice");
    ensure_generator_window(cuda_ordinal_);
    throw_if_cuda_error(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking),
                         "cudaStreamCreateWithFlags");
    try {
      throw_if_cuda_error(cudaMalloc(&matches_, static_cast<std::size_t>(capacity_) * sizeof(GpuMatch)),
                           "cudaMalloc(matches)");
      throw_if_cuda_error(cudaMalloc(&match_count_, sizeof(std::uint32_t)), "cudaMalloc(match count)");
      throw_if_cuda_error(cudaMalloc(&valid_count_, sizeof(std::uint32_t)), "cudaMalloc(valid count)");
      throw_if_cuda_error(cudaMalloc(&pipeline_failure_count_, sizeof(std::uint32_t)),
                           "cudaMalloc(pipeline failure count)");
      group_capacity_ = (capacity_ + kCandidatesPerThread - 1U) / kCandidatesPerThread;
      workspace_capacity_ = group_capacity_ * kCandidatesPerThread;
      throw_if_cuda_error(
          cudaMalloc(&points_, static_cast<std::size_t>(workspace_capacity_) * sizeof(Point)),
          "cudaMalloc(points)");
      throw_if_cuda_error(
          cudaMalloc(&prefix_products_, static_cast<std::size_t>(workspace_capacity_) * sizeof(Field)),
          "cudaMalloc(prefix products)");
      throw_if_cuda_error(
          cudaMalloc(&z_inverses_, static_cast<std::size_t>(workspace_capacity_) * sizeof(Field)),
          "cudaMalloc(Z inverses)");
      throw_if_cuda_error(
          cudaMalloc(&group_counts_, static_cast<std::size_t>(group_capacity_) * sizeof(std::uint32_t)),
          "cudaMalloc(group counts)");
    } catch (...) {
      release();
      throw;
    }
  }

  ~DeviceBuffers() { release(); }

  DeviceBuffers(const DeviceBuffers&) = delete;
  DeviceBuffers& operator=(const DeviceBuffers&) = delete;

  BatchOutcome execute(const DeviceKey& base_key, const std::uint64_t begin_offset,
                       const std::uint32_t candidate_count, const DevicePattern pattern,
                       const bool collect_all) {
    if (candidate_count == 0 || candidate_count > capacity_) {
      throw std::invalid_argument("CUDA batch candidate count exceeds allocated capacity");
    }
    throw_if_cuda_error(cudaMemsetAsync(matches_, 0, static_cast<std::size_t>(capacity_) * sizeof(GpuMatch),
                                        stream_), "cudaMemsetAsync(matches)");
    throw_if_cuda_error(cudaMemsetAsync(match_count_, 0, sizeof(std::uint32_t), stream_),
                         "cudaMemsetAsync(match count)");
    throw_if_cuda_error(cudaMemsetAsync(valid_count_, 0, sizeof(std::uint32_t), stream_),
                         "cudaMemsetAsync(valid count)");
    throw_if_cuda_error(cudaMemsetAsync(pipeline_failure_count_, 0, sizeof(std::uint32_t), stream_),
                         "cudaMemsetAsync(pipeline failure count)");
    const std::uint32_t group_count =
        (candidate_count + kCandidatesPerThread - 1U) / kCandidatesPerThread;
    throw_if_cuda_error(
        cudaMemsetAsync(group_counts_, 0,
                        static_cast<std::size_t>(group_count) * sizeof(std::uint32_t), stream_),
        "cudaMemsetAsync(group counts)");
    const std::uint32_t blocks = (group_count + kThreadsPerBlock - 1U) / kThreadsPerBlock;
    point_progression_kernel<<<blocks, kThreadsPerBlock, 0, stream_>>>(
        base_key, begin_offset, candidate_count, group_count, points_, group_counts_,
        pipeline_failure_count_);
    throw_if_cuda_error(cudaGetLastError(), "CUDA point-progression kernel launch");
    batch_inversion_kernel<<<blocks, kThreadsPerBlock, 0, stream_>>>(
        candidate_count, group_count, points_, group_counts_, prefix_products_, z_inverses_);
    throw_if_cuda_error(cudaGetLastError(), "CUDA batch-inversion kernel launch");
    const std::uint32_t hash_blocks =
        (workspace_capacity_ + kHashThreadsPerBlock - 1U) / kHashThreadsPerBlock;
    address_match_kernel<<<hash_blocks, kHashThreadsPerBlock, 0, stream_>>>(
        base_key, begin_offset, candidate_count, group_count, pattern, collect_all, points_, z_inverses_,
        group_counts_, matches_, match_count_, valid_count_, pipeline_failure_count_, capacity_);
    throw_if_cuda_error(cudaGetLastError(), "CUDA address-match kernel launch");

    std::uint32_t host_match_count = 0;
    std::uint32_t host_valid_count = 0;
    std::uint32_t host_pipeline_failure_count = 0;
    throw_if_cuda_error(cudaMemcpyAsync(&host_match_count, match_count_, sizeof(host_match_count),
                                        cudaMemcpyDeviceToHost, stream_), "cudaMemcpyAsync(match count)");
    throw_if_cuda_error(cudaMemcpyAsync(&host_valid_count, valid_count_, sizeof(host_valid_count),
                                        cudaMemcpyDeviceToHost, stream_), "cudaMemcpyAsync(valid count)");
    throw_if_cuda_error(cudaMemcpyAsync(&host_pipeline_failure_count, pipeline_failure_count_,
                                        sizeof(host_pipeline_failure_count), cudaMemcpyDeviceToHost,
                                        stream_), "cudaMemcpyAsync(pipeline failure count)");
    throw_if_cuda_error(cudaStreamSynchronize(stream_), "CUDA vanity kernel synchronization");
    if (host_pipeline_failure_count != 0) {
      throw std::runtime_error("CUDA cryptographic pipeline failed for a valid scalar");
    }
    if (host_match_count > capacity_) {
      throw std::runtime_error("CUDA kernel reported more matches than its bounded result buffer");
    }

    BatchOutcome result;
    result.valid_count = host_valid_count;
    result.matches.resize(host_match_count);
    if (host_match_count != 0) {
      throw_if_cuda_error(cudaMemcpyAsync(result.matches.data(), matches_,
                                          static_cast<std::size_t>(host_match_count) * sizeof(GpuMatch),
                                          cudaMemcpyDeviceToHost, stream_), "cudaMemcpyAsync(matches)");
      throw_if_cuda_error(cudaStreamSynchronize(stream_), "CUDA match synchronization");
    }
    return result;
  }

 private:
  void release() noexcept {
    if (cuda_ordinal_ >= 0) cudaSetDevice(cuda_ordinal_);
    if (stream_ != nullptr) {
      if (matches_ != nullptr) {
        cudaMemsetAsync(matches_, 0, static_cast<std::size_t>(capacity_) * sizeof(GpuMatch), stream_);
      }
      if (match_count_ != nullptr) cudaMemsetAsync(match_count_, 0, sizeof(std::uint32_t), stream_);
      if (valid_count_ != nullptr) cudaMemsetAsync(valid_count_, 0, sizeof(std::uint32_t), stream_);
      if (pipeline_failure_count_ != nullptr) {
        cudaMemsetAsync(pipeline_failure_count_, 0, sizeof(std::uint32_t), stream_);
      }
      cudaStreamSynchronize(stream_);
    }
    if (matches_ != nullptr) cudaFree(matches_);
    if (match_count_ != nullptr) cudaFree(match_count_);
    if (valid_count_ != nullptr) cudaFree(valid_count_);
    if (pipeline_failure_count_ != nullptr) cudaFree(pipeline_failure_count_);
    if (points_ != nullptr) cudaFree(points_);
    if (prefix_products_ != nullptr) cudaFree(prefix_products_);
    if (z_inverses_ != nullptr) cudaFree(z_inverses_);
    if (group_counts_ != nullptr) cudaFree(group_counts_);
    if (stream_ != nullptr) cudaStreamDestroy(stream_);
    matches_ = nullptr;
    match_count_ = nullptr;
    valid_count_ = nullptr;
    pipeline_failure_count_ = nullptr;
    points_ = nullptr;
    prefix_products_ = nullptr;
    z_inverses_ = nullptr;
    group_counts_ = nullptr;
    stream_ = nullptr;
  }

  int cuda_ordinal_ = -1;
  std::uint32_t capacity_ = 0;
  cudaStream_t stream_ = nullptr;
  GpuMatch* matches_ = nullptr;
  std::uint32_t* match_count_ = nullptr;
  std::uint32_t* valid_count_ = nullptr;
  std::uint32_t* pipeline_failure_count_ = nullptr;
  std::uint32_t group_capacity_ = 0;
  std::uint32_t workspace_capacity_ = 0;
  Point* points_ = nullptr;
  Field* prefix_products_ = nullptr;
  Field* z_inverses_ = nullptr;
  std::uint32_t* group_counts_ = nullptr;
};

DevicePattern to_device_pattern(const PatternSpec& pattern) {
  const PatternValidation validation = validate_pattern(pattern);
  if (!validation.valid) throw std::invalid_argument(validation.error);
  DevicePattern result{};
  result.mode = pattern.mode == PatternMode::literal ? kPatternLiteral : kPatternRepeat;
  switch (pattern.position) {
    case PatternPosition::prefix: result.position = kPatternPrefix; break;
    case PatternPosition::suffix: result.position = kPatternSuffix; break;
    case PatternPosition::both: result.position = kPatternBoth; break;
  }
  if (pattern.mode == PatternMode::literal) {
    result.prefix_length = static_cast<std::uint8_t>(pattern.literal_prefix.size());
    result.suffix_length = static_cast<std::uint8_t>(pattern.literal_suffix.size());
    if (!pattern.literal_prefix.empty()) {
      std::memcpy(result.prefix, pattern.literal_prefix.data(), pattern.literal_prefix.size());
    }
    if (!pattern.literal_suffix.empty()) {
      std::memcpy(result.suffix, pattern.literal_suffix.data(), pattern.literal_suffix.size());
    }
  } else {
    result.prefix_length = static_cast<std::uint8_t>(pattern.repeat_prefix_length);
    result.suffix_length = static_cast<std::uint8_t>(pattern.repeat_suffix_length);
  }
  return result;
}

DeviceKey to_device_key(const PrivateKeyBytes& key) {
  DeviceKey result{};
  std::memcpy(result.bytes, key.data(), key.size());
  return result;
}

class SecureDeviceKey final {
 public:
  explicit SecureDeviceKey(const PrivateKeyBytes& key) {
    std::memcpy(value_.bytes, key.data(), key.size());
  }
  ~SecureDeviceKey() { secure_zero(&value_, sizeof(value_)); }
  SecureDeviceKey(const SecureDeviceKey&) = delete;
  SecureDeviceKey& operator=(const SecureDeviceKey&) = delete;
  const DeviceKey& value() const noexcept { return value_; }

 private:
  DeviceKey value_{};
};

std::string match_address_string(const GpuMatch& match) {
  std::size_t length = 0;
  while (length < sizeof(match.address) && match.address[length] != '\0') ++length;
  if (length == sizeof(match.address)) throw std::runtime_error("CUDA match address is not NUL terminated");
  return std::string(match.address, length);
}

std::vector<CudaTestCandidate> copy_test_candidates(BatchOutcome& batch) {
  std::vector<CudaTestCandidate> result;
  try {
    result.reserve(batch.matches.size());
    for (const auto& match : batch.matches) {
      CudaTestCandidate candidate{};
      std::memcpy(candidate.private_key.data(), match.private_key, candidate.private_key.size());
      candidate.address = match_address_string(match);
      result.push_back(std::move(candidate));
    }
  } catch (...) {
    secure_zero_matches(batch.matches);
    throw;
  }
  secure_zero_matches(batch.matches);
  return result;
}

}  // namespace

CudaBackend::CudaBackend(const int cuda_ordinal) : cuda_ordinal_(cuda_ordinal) {
  info_.id = "cuda-" + std::to_string(cuda_ordinal_);
  info_.name = "CUDA GPU";
  info_.kind = BackendKind::cuda;
  info_.threads = 0;
  info_.gpu_index = cuda_ordinal_;
}

CudaBackend::~CudaBackend() {
  stop();
  secure_zero(base_key_.data(), base_key_.size());
}

bool CudaBackend::initialize() {
  if (cuda_ordinal_ < 0) return false;
  if (cudaSetDevice(cuda_ordinal_) != cudaSuccess) return false;
  cudaDeviceProp properties{};
  if (cudaGetDeviceProperties(&properties, cuda_ordinal_) != cudaSuccess) return false;
  info_.name = properties.name[0] == '\0' ? "CUDA GPU" : properties.name;
  info_.threads = properties.multiProcessorCount > 0
                      ? static_cast<std::uint32_t>(properties.multiProcessorCount) : 0;
  int reported_clock_rate = 0;
  if (cudaDeviceGetAttribute(&reported_clock_rate, cudaDevAttrClockRate, cuda_ordinal_) !=
      cudaSuccess) {
    cudaGetLastError();
    reported_clock_rate = 0;
  }
  const std::uint64_t clock_rate = reported_clock_rate > 0
                                       ? static_cast<std::uint64_t>(reported_clock_rate) : 1;
  info_.performance_score = static_cast<std::uint64_t>(info_.threads) * clock_rate;
  return true;
}

void CudaBackend::start(const SearchTask& task, const PrivateKeyBytes& base_key,
                        SearchSpaceAllocator& allocator, std::shared_ptr<SearchControl> control,
                        CandidateCallback candidate_callback, BackendErrorCallback error_callback) {
  if (!control) throw std::invalid_argument("CUDA backend requires a task search control");
  if (!candidate_callback) throw std::invalid_argument("CUDA backend requires a candidate callback");
  stop();
  task_ = task;
  base_key_ = base_key;
  allocator_ = &allocator;
  control_ = std::move(control);
  candidate_callback_ = std::move(candidate_callback);
  error_callback_ = std::move(error_callback);
  checked_.store(0, std::memory_order_relaxed);
  stop_requested_.store(false, std::memory_order_release);
  paused_.store(false, std::memory_order_release);
  error_reported_.store(false, std::memory_order_release);
  {
    std::lock_guard lock(stats_mutex_);
    last_sample_checked_ = 0;
    last_sample_time_ = std::chrono::steady_clock::now();
    last_hashrate_ = 0;
  }
  try {
    worker_ = std::thread(&CudaBackend::worker_loop, this);
  } catch (...) {
    stop_requested_.store(true, std::memory_order_release);
    candidate_callback_ = {};
    error_callback_ = {};
    allocator_ = nullptr;
    secure_zero(base_key_.data(), base_key_.size());
    throw;
  }
}

void CudaBackend::pause() {
  if (stop_requested_.load(std::memory_order_acquire)) return;
  paused_.store(true, std::memory_order_release);
  pause_condition_.notify_all();
  std::unique_lock lock(pause_mutex_);
  pause_condition_.wait(lock, [this] {
    return pause_acknowledged_ || stop_requested_.load(std::memory_order_acquire) ||
           control_->cancel_requested.load(std::memory_order_acquire);
  });
}

void CudaBackend::resume() {
  paused_.store(false, std::memory_order_release);
  pause_condition_.notify_all();
}

void CudaBackend::stop() {
  stop_requested_.store(true, std::memory_order_release);
  paused_.store(false, std::memory_order_release);
  pause_condition_.notify_all();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
  if (worker_.joinable()) return;
  {
    std::lock_guard lock(pause_mutex_);
    pause_acknowledged_ = false;
  }
  candidate_callback_ = {};
  error_callback_ = {};
  allocator_ = nullptr;
  secure_zero(base_key_.data(), base_key_.size());
}

BackendStats CudaBackend::get_stats() {
  BackendStats stats;
  stats.checked = checked_.load(std::memory_order_relaxed);
  stats.running = !stop_requested_.load(std::memory_order_acquire);
  stats.paused = paused_.load(std::memory_order_acquire);
  std::lock_guard lock(stats_mutex_);
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - last_sample_time_).count();
  if (stats.running && !stats.paused && seconds >= 0.25) {
    stats.hashrate = static_cast<double>(stats.checked - last_sample_checked_) / seconds;
    last_hashrate_ = stats.hashrate;
    last_sample_checked_ = stats.checked;
    last_sample_time_ = now;
  } else if (stats.running && !stats.paused) {
    stats.hashrate = last_hashrate_;
  }
  return stats;
}

bool CudaBackend::wait_while_paused() {
  if (!paused_.load(std::memory_order_acquire)) {
    return !stop_requested_.load(std::memory_order_acquire) &&
           !control_->cancel_requested.load(std::memory_order_acquire);
  }
  std::unique_lock lock(pause_mutex_);
  pause_acknowledged_ = true;
  pause_condition_.notify_all();
  pause_condition_.wait(lock, [this] {
    return stop_requested_.load(std::memory_order_acquire) ||
           control_->cancel_requested.load(std::memory_order_acquire) ||
           !paused_.load(std::memory_order_acquire);
  });
  pause_acknowledged_ = false;
  pause_condition_.notify_all();
  return !stop_requested_.load(std::memory_order_acquire) &&
         !control_->cancel_requested.load(std::memory_order_acquire);
}

void CudaBackend::report_error(const std::string& message) noexcept {
  stop_requested_.store(true, std::memory_order_release);
  paused_.store(false, std::memory_order_release);
  if (control_) control_->cancel_requested.store(true, std::memory_order_release);
  pause_condition_.notify_all();
  if (error_reported_.exchange(true, std::memory_order_acq_rel)) return;
  try {
    if (error_callback_) error_callback_(message);
  } catch (...) {
    // An error reporter must never take down the CUDA worker thread.
  }
}

void CudaBackend::worker_loop() noexcept {
  try {
    throw_if_cuda_error(cudaSetDevice(cuda_ordinal_), "cudaSetDevice(worker)");
    DeviceBuffers buffers(cuda_ordinal_, kSearchBatchSize);
    const DevicePattern pattern = to_device_pattern(task_.pattern);
    const SecureDeviceKey base_key(base_key_);

    while (!stop_requested_.load(std::memory_order_acquire) &&
           !control_->cancel_requested.load(std::memory_order_acquire)) {
      if (!wait_while_paused()) break;
      const auto range = allocator_->acquire(kSearchBatchSize);
      if (!range) {
        stop_requested_.store(true, std::memory_order_release);
        break;
      }
      BatchOutcome batch = buffers.execute(base_key.value(), range->begin_offset,
                                           static_cast<std::uint32_t>(range->count), pattern, false);
      checked_.fetch_add(batch.valid_count, std::memory_order_relaxed);
      bool keep_searching = batch.valid_count == range->count;
      try {
        for (const auto& match : batch.matches) {
          if (!wait_while_paused()) {
            keep_searching = false;
            break;
          }
          PrivateKeyBytes private_key{};
          std::memcpy(private_key.data(), match.private_key, private_key.size());
          bool accepted = false;
          try {
            const std::string address = match_address_string(match);
            accepted = candidate_callback_(std::move(private_key), address);
          } catch (...) {
            secure_zero(private_key.data(), private_key.size());
            throw;
          }
          secure_zero(private_key.data(), private_key.size());
          if (!accepted) {
            keep_searching = false;
            stop_requested_.store(true, std::memory_order_release);
            break;
          }
        }
      } catch (...) {
        secure_zero_matches(batch.matches);
        throw;
      }
      secure_zero_matches(batch.matches);
      if (!keep_searching) {
        stop_requested_.store(true, std::memory_order_release);
        break;
      }
    }
  } catch (const std::exception& exception) {
    report_error(exception.what());
  } catch (...) {
    report_error("unknown CUDA worker failure");
  }
  stop_requested_.store(true, std::memory_order_release);
  paused_.store(false, std::memory_order_release);
  pause_condition_.notify_all();
  secure_zero(base_key_.data(), base_key_.size());
}

void append_cuda_backends(std::vector<std::unique_ptr<SearchBackend>>& backends) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count <= 0) {
    cudaGetLastError();
    return;
  }
  for (int cuda_ordinal = 0; cuda_ordinal < device_count; ++cuda_ordinal) {
    backends.push_back(std::make_unique<CudaBackend>(cuda_ordinal));
  }
}

bool cuda_driver_compatible(std::string& error) noexcept {
  int runtime_version = 0;
  const cudaError_t runtime_status = cudaRuntimeGetVersion(&runtime_version);
  if (runtime_status != cudaSuccess) {
    error = "Unable to query the CUDA Runtime version.";
    cudaGetLastError();
    return false;
  }
  if (runtime_version < VANITYFORGE_MIN_CUDA_DRIVER_API) {
    error = "This Vanylith executable does not contain the required CUDA 13.x Runtime.";
    return false;
  }

  int driver_version = 0;
  const cudaError_t driver_status = cudaDriverGetVersion(&driver_version);
  if (driver_status == cudaErrorInsufficientDriver ||
      (driver_status == cudaSuccess && driver_version < VANITYFORGE_MIN_CUDA_DRIVER_API)) {
    error = "NVIDIA driver is too old for this Vanylith CUDA build. "
            "CUDA 13.x Driver API support (NVIDIA Release 580 or newer) is required.";
    cudaGetLastError();
    return false;
  }
  if (driver_status != cudaSuccess) {
    error = std::string("NVIDIA CUDA driver initialization failed: ") +
            cudaGetErrorString(driver_status);
    cudaGetLastError();
    return false;
  }

  error.clear();
  return true;
}

bool cuda_runtime_available_for_tests() noexcept {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess) {
    cudaGetLastError();
    return false;
  }
  return device_count > 0;
}

std::vector<CudaTestCandidate> cuda_derive_candidates_for_test(
    const int cuda_ordinal, const PrivateKeyBytes& base_key, const std::uint64_t begin_offset,
    const std::uint32_t count) {
  DeviceBuffers buffers(cuda_ordinal, count);
  DevicePattern ignored_pattern{};
  BatchOutcome batch = buffers.execute(to_device_key(base_key), begin_offset, count, ignored_pattern, true);
  return copy_test_candidates(batch);
}

std::vector<CudaTestCandidate> cuda_search_candidates_for_test(
    const int cuda_ordinal, const PrivateKeyBytes& base_key, const std::uint64_t begin_offset,
    const std::uint32_t count, const PatternSpec& pattern) {
  DeviceBuffers buffers(cuda_ordinal, count);
  BatchOutcome batch = buffers.execute(to_device_key(base_key), begin_offset, count,
                                       to_device_pattern(pattern), false);
  return copy_test_candidates(batch);
}

}  // namespace vanityforge
