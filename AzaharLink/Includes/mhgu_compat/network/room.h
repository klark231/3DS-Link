#pragma once
#include <array>
#include <vector>
#include "common/common_types.h"
namespace Network {
using MacAddress = std::array<u8, 6>;
constexpr MacAddress BroadcastMac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
} // namespace Network
