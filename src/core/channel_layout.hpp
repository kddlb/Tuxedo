#pragma once

#include <cstdint>
#include <vector>

namespace tuxedo {

namespace channel_layout {

constexpr uint64_t kFrontLeft = 1ull << 0;
constexpr uint64_t kFrontRight = 1ull << 1;
constexpr uint64_t kFrontCenter = 1ull << 2;
constexpr uint64_t kLfe = 1ull << 3;
constexpr uint64_t kBackLeft = 1ull << 4;
constexpr uint64_t kBackRight = 1ull << 5;
constexpr uint64_t kFrontLeftOfCenter = 1ull << 6;
constexpr uint64_t kFrontRightOfCenter = 1ull << 7;
constexpr uint64_t kBackCenter = 1ull << 8;
constexpr uint64_t kSideLeft = 1ull << 9;
constexpr uint64_t kSideRight = 1ull << 10;

constexpr uint64_t kMono = kFrontCenter;
constexpr uint64_t kStereo = kFrontLeft | kFrontRight;

uint64_t default_for_channels(uint32_t channels);
std::vector<uint64_t> ordered_channels(uint64_t layout, uint32_t channels);

} // namespace channel_layout

} // namespace tuxedo
