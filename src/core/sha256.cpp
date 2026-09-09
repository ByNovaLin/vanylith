#include "core/sha256.h"

#include <Windows.h>
#include <bcrypt.h>

#include <limits>
#include <stdexcept>
#include <vector>

namespace vanityforge {
namespace {

class AlgorithmHandle final {
 public:
  AlgorithmHandle() {
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
      throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA-256) failed");
    }
  }
  ~AlgorithmHandle() { BCryptCloseAlgorithmProvider(handle_, 0); }
  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class HashHandle final {
 public:
  HashHandle(BCRYPT_ALG_HANDLE algorithm, std::vector<std::uint8_t>& object) {
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm, &handle_, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0))) {
      throw std::runtime_error("BCryptCreateHash(SHA-256) failed");
    }
  }
  ~HashHandle() { BCryptDestroyHash(handle_); }
  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;
  BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_ = nullptr;
};

ULONG get_ulong_property(BCRYPT_ALG_HANDLE algorithm, const wchar_t* name) {
  ULONG value = 0;
  ULONG copied = 0;
  if (!BCRYPT_SUCCESS(BCryptGetProperty(
          algorithm, name, reinterpret_cast<PUCHAR>(&value), sizeof(value), &copied, 0)) ||
      copied != sizeof(value)) {
    throw std::runtime_error("BCryptGetProperty failed");
  }
  return value;
}

}  // namespace

Hash256 sha256(const std::span<const std::uint8_t> input) {
  if (input.size() > std::numeric_limits<ULONG>::max()) {
    throw std::invalid_argument("SHA-256 input is too large");
  }

  AlgorithmHandle algorithm;
  const ULONG object_size = get_ulong_property(algorithm.get(), BCRYPT_OBJECT_LENGTH);
  const ULONG digest_size = get_ulong_property(algorithm.get(), BCRYPT_HASH_LENGTH);
  if (digest_size != Hash256{}.size()) {
    throw std::runtime_error("Unexpected SHA-256 digest size");
  }

  std::vector<std::uint8_t> object(object_size);
  HashHandle hash(algorithm.get(), object);
  if (!BCRYPT_SUCCESS(BCryptHashData(
          hash.get(), const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0))) {
    throw std::runtime_error("BCryptHashData(SHA-256) failed");
  }

  Hash256 output{};
  if (!BCRYPT_SUCCESS(
          BCryptFinishHash(hash.get(), output.data(), static_cast<ULONG>(output.size()), 0))) {
    throw std::runtime_error("BCryptFinishHash(SHA-256) failed");
  }
  return output;
}

}  // namespace vanityforge

