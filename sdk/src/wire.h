#pragma once
#include <array>
#include <cstdint>
#include "spacedesk_protocol.hpp"

namespace sd {
constexpr size_t header_size = protocol::header_size;
constexpr uint32_t max_payload = 64 * 1024 * 1024;
constexpr uint64_t max_pixels = 16 * 1024 * 1024;
using Header = std::array<uint8_t, header_size>;
inline bool valid_size(uint32_t w, uint32_t h) {
    return w && h && w <= 8192 && h <= 8192 && uint64_t(w) * h <= max_pixels;
}
}
