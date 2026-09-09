#include "core/search_space.h"

#include "core/tron_address.h"

#include <algorithm>
#include <limits>

namespace vanityforge {

SearchSpaceAllocator::SearchSpaceAllocator(const std::uint64_t maximum_offsets) noexcept
    : maximum_offsets_(maximum_offsets) {}

std::optional<SearchRange> SearchSpaceAllocator::acquire(const std::uint64_t requested_count) noexcept {
  if (requested_count == 0) {
    return std::nullopt;
  }

  std::uint64_t current = next_.load(std::memory_order_relaxed);
  for (;;) {
    if (current >= maximum_offsets_) {
      return std::nullopt;
    }
    const std::uint64_t remaining = maximum_offsets_ - current;
    const std::uint64_t granted = std::min(requested_count, remaining);
    const std::uint64_t desired = current + granted;
    if (next_.compare_exchange_weak(current, desired, std::memory_order_relaxed)) {
      return SearchRange{current, granted};
    }
  }
}

std::uint64_t SearchSpaceAllocator::issued() const noexcept {
  return next_.load(std::memory_order_relaxed);
}

std::optional<PrivateKeyBytes> candidate_at_offset(const PrivateKeyBytes& base,
                                                   const std::uint64_t offset) noexcept {
  PrivateKeyBytes candidate = base;
  std::uint64_t carry = offset;
  for (std::size_t i = candidate.size(); i > 0 && carry != 0; --i) {
    const std::uint64_t sum = static_cast<std::uint64_t>(candidate[i - 1]) + (carry & 0xffU);
    candidate[i - 1] = static_cast<std::uint8_t>(sum & 0xffU);
    carry = (carry >> 8U) + (sum >> 8U);
  }
  if (carry != 0 || !is_valid_private_key(candidate)) {
    return std::nullopt;
  }
  return candidate;
}

}  // namespace vanityforge

