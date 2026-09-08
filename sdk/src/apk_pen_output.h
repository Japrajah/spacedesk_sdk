#pragma once
#include "touchpad.h"

namespace sd::native {
// Role alias for APK V1. No recovered original class name, superclass or VMT.
// Original constructor calls d(width,height); these are its only state fields.
class APKPenOutput {
    uint32_t width_=0,height_=0;
public:
    APKPenOutput(uint32_t width,uint32_t height) noexcept { SurfaceSet(width,height); }
    void SurfaceSet(uint32_t width,uint32_t height) noexcept { width_=width;height_=height; }
    protocol::PenHeader Hover(const SAMotionEvent& event) const noexcept {
        protocol::PenHeader packet{};
        packet.surface_width=width_;packet.surface_height=height_;
        packet.x=float(event.X());packet.y=float(event.Y());packet.in_range=1;
        return packet;
    }
    protocol::PenHeader Contact(const SAMotionEvent& event) const noexcept {
        protocol::PenHeader packet{};
        packet.surface_width=width_;packet.surface_height=height_;
        packet.x=float(event.X());packet.y=float(event.Y());packet.pressure=event.Pressure();
        packet.contact=event.Action()==1?0:1;return packet;
    }
    protocol::MouseHeader Button(bool released) const noexcept {
        protocol::MouseHeader packet{};packet.button_flags=released?16:8;return packet;
    }
};
}
