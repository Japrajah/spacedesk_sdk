#pragma once
#include "wire.h"
#include <vector>
namespace sd::native {
struct SANetPacket {
    Header header{};
    std::vector<uint8_t> payload;
};
}
