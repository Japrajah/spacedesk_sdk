#pragma once
#include <array>
#include <cstdint>
namespace sd::native {
// APK N1.k / Q2.a..d texture coordinates. Descriptive SDK helper.
inline std::array<float,2> DisplaySourceUv(uint32_t orientation,float u,float v) noexcept {
    switch(orientation) {
    case 2:return {v,1-u};
    case 3:return {1-u,1-v};
    case 4:return {1-v,u};
    default:return {u,v};
    }
}
inline bool DisplaySideways(uint32_t orientation) noexcept { return orientation==2 || orientation==4; }
}
