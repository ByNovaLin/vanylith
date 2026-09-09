#pragma once

#include "core/types.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace vanityforge {

struct SearchRange final {
  std::uint64_t begin_offset = 0;
  std::uint64_t count = 0;
};

class SearchSpaceAllocator final {
 public:
  explicit SearchSpaceAllocator(std::uint64_t maximum_offsets = UINT64_MAX) noexcept;
  std::optional<SearchRange> acquire(std::uint64_t requested_count) noexcept;
  std::uint64_t issued() const noexcept;

 private:
  std::atomic<std::uint64_t> next_{0};
  std::uint64_t maximum_offsets_;
};

std::optional<PrivateKeyBytes> candidate_at_offset(const PrivateKeyBytes& base,
                                                   std::uint64_t offset) noexcept;

}  // namespace vanityforge

