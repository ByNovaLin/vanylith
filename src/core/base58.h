#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vanityforge {

inline constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool is_base58_character(char value) noexcept;
std::string base58_encode(std::span<const std::uint8_t> input);

}  // namespace vanityforge

