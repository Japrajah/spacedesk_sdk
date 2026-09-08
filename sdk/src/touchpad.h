#pragma once
#include "spacedesk_protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "spacedesk_sdk.h"

namespace sd::native {
// Portable boundary after coordinate mapping. Order is MotionEvent's pointer
// index order, not pointer-ID sort order; IDs are narrowed only at serialization.
struct SAMotionContact {
    uint32_t pointer_id = 0;
    int32_t x = 0, y = 0;
    bool mapped = true;
};

// Name confirmed by SAActivityDisplay's "SAMotionEvent out of bounds" logs for
// APK C0677b2. No recovered superclass or virtual methods. The portable overload
// accepts MotionEvent coordinates already read by the frontend; b/c mapping and
// a() primary/list selection retain the recovered arithmetic and ordering.
class SAMotionEvent {
    uint32_t action_ = 0, primary_id_ = 0;
    int32_t x_ = 0, y_ = 0;
    float pressure_ = 0;
    std::vector<SAMotionContact> contacts_;
    bool valid_ = false;
    void Select(const SAMotionContact*,size_t);
public:
    SAMotionEvent(uint32_t raw_action, const SAMotionContact* contacts, size_t count);
    SAMotionEvent(uint32_t raw_action,const sd_motion_geometry&,const SAMotionContact*,size_t);
    SAMotionEvent(const sd_motion_geometry&,const sd_motion_event&);
    SAMotionEvent(const sd_motion_event&,uint32_t mapped_mask);
    static bool Map(const sd_motion_geometry&,int32_t& x,int32_t& y) noexcept;
    bool Valid() const noexcept { return valid_; }
    uint32_t Action() const noexcept { return action_; }
    uint32_t PrimaryId() const noexcept { return primary_id_; }
    int32_t X() const noexcept { return x_; }
    int32_t Y() const noexcept { return y_; }
    float Pressure() const noexcept { return pressure_; }
    const std::vector<SAMotionContact>& Contacts() const noexcept { return contacts_; }
};

// Explicit role alias for obfuscated APK W1, not a recovered native class name.
// W1 has no inheritance: exactly mode + width + height, no gesture state/timer.
// Relative touch is a type-12 protocol mode, not client-side MouseHeader gestures.
class APKTouchOutput {
    uint32_t mode_ = 0, width_ = 0, height_ = 0;
public:
    APKTouchOutput(uint32_t width, uint32_t height) noexcept : width_(width), height_(height) {}
    void ModeSet(uint32_t mode) noexcept { mode_ = mode; }
    void SurfaceSet(uint32_t width, uint32_t height) noexcept { width_ = width; height_ = height; }
    std::optional<protocol::TouchHeader> Build(const SAMotionEvent& event, uint64_t wallclock_ms) const;
};
}
