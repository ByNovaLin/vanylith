#include "test_framework.h"

#include "core/search_space.h"

#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

VF_TEST("Search allocator returns non-overlapping ranges across threads") {
  vanityforge::SearchSpaceAllocator allocator(10000);
  std::vector<vanityforge::SearchRange> ranges;
  std::mutex ranges_mutex;
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 8; ++worker) {
    threads.emplace_back([&] {
      for (;;) {
        const auto range = allocator.acquire(37);
        if (!range) break;
        std::lock_guard lock(ranges_mutex);
        ranges.push_back(*range);
      }
    });
  }
  for (auto& thread : threads) thread.join();

  std::set<std::uint64_t> offsets;
  for (const auto& range : ranges) {
    for (std::uint64_t offset = range.begin_offset; offset < range.begin_offset + range.count;
         ++offset) {
      VF_REQUIRE(offsets.insert(offset).second);
    }
  }
  VF_REQUIRE_EQ(offsets.size(), static_cast<std::size_t>(10000));
  VF_REQUIRE_EQ(allocator.issued(), static_cast<std::uint64_t>(10000));
}

VF_TEST("Candidate offsets are big-endian additions without overlap") {
  vanityforge::PrivateKeyBytes base{};
  base.back() = 1;
  const auto first = vanityforge::candidate_at_offset(base, 0);
  const auto second = vanityforge::candidate_at_offset(base, 1);
  const auto carried = vanityforge::candidate_at_offset(base, 255);
  VF_REQUIRE(first.has_value());
  VF_REQUIRE(second.has_value());
  VF_REQUIRE(carried.has_value());
  VF_REQUIRE_EQ(first->back(), static_cast<std::uint8_t>(1));
  VF_REQUIRE_EQ(second->back(), static_cast<std::uint8_t>(2));
  VF_REQUIRE_EQ((*carried)[30], static_cast<std::uint8_t>(1));
  VF_REQUIRE_EQ((*carried)[31], static_cast<std::uint8_t>(0));
}

VF_TEST("Search allocator keeps heterogeneous device batches disjoint at exhaustion") {
  vanityforge::SearchSpaceAllocator allocator(1003);
  const auto zero = allocator.acquire(0);
  VF_REQUIRE(!zero.has_value());

  std::vector<vanityforge::SearchRange> ranges;
  const std::array<std::uint64_t, 4> cpu_batches{17, 31, 7, 13};
  const std::array<std::uint64_t, 3> gpu_batches{256, 128, 512};
  for (std::size_t index = 0;; ++index) {
    const std::uint64_t requested = index % 2 == 0
        ? cpu_batches[(index / 2) % cpu_batches.size()]
        : gpu_batches[(index / 2) % gpu_batches.size()];
    const auto range = allocator.acquire(requested);
    if (!range) break;
    ranges.push_back(*range);
  }

  std::set<std::uint64_t> offsets;
  for (const auto& range : ranges) {
    VF_REQUIRE(range.count > 0);
    VF_REQUIRE(range.begin_offset <= 1003 - range.count);
    for (std::uint64_t offset = range.begin_offset; offset < range.begin_offset + range.count;
         ++offset) {
      VF_REQUIRE(offsets.insert(offset).second);
    }
  }
  VF_REQUIRE_EQ(offsets.size(), static_cast<std::size_t>(1003));
  VF_REQUIRE_EQ(allocator.issued(), static_cast<std::uint64_t>(1003));
  VF_REQUIRE(!allocator.acquire(std::numeric_limits<std::uint64_t>::max()).has_value());
}

VF_TEST("Candidate offsets reject secp256k1 scalar overflow") {
  vanityforge::PrivateKeyBytes order{
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
      0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
      0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};
  VF_REQUIRE(!vanityforge::candidate_at_offset(order, 0).has_value());
  --order.back();
  VF_REQUIRE(vanityforge::candidate_at_offset(order, 0).has_value());
  VF_REQUIRE(!vanityforge::candidate_at_offset(order, 1).has_value());
}
