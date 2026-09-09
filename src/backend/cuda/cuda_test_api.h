#pragma once

#include "core/pattern.h"
#include "core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vanityforge {

// Diagnostic-only entry points used by the CUDA CTest target. They execute
// the same device pipeline as CudaBackend and make no CPU cryptographic calls.
struct CudaTestCandidate final {
  PrivateKeyBytes private_key{};
  std::string address;
};

bool cuda_runtime_available_for_tests() noexcept;
std::vector<CudaTestCandidate> cuda_derive_candidates_for_test(
    int cuda_ordinal, const PrivateKeyBytes& base_key, std::uint64_t begin_offset,
    std::uint32_t count);
std::vector<CudaTestCandidate> cuda_search_candidates_for_test(
    int cuda_ordinal, const PrivateKeyBytes& base_key, std::uint64_t begin_offset,
    std::uint32_t count, const PatternSpec& pattern);

}  // namespace vanityforge
