#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace nanhang_weighted_fetch {

namespace fs = std::filesystem;

inline constexpr std::size_t kSamples = 18;
inline constexpr std::size_t kDimensions = 54;

struct Returned54 {
  std::array<std::int64_t, kSamples> times{};
  std::array<float, kDimensions> values{};
};

struct FetchTiming {
  std::uint64_t locate_ns = 0;
  std::uint64_t decode_ns = 0;
};

}
