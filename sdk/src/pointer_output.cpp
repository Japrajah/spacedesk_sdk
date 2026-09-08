#include "pointer_output.h"
#include "frame_decoder.h"
#include <ctime>
#include <limits>

namespace sd::native {
sd_result SAMouseOutput::Emit() {
    uint32_t flags = (delta_x_ || delta_y_) ? SD_MOUSE_MOVE : 0;
    flags |= (changes_ & 0x3f) << 1;
    const bool wheel = (changes_ & 0x400) != 0;
    if (wheel) flags |= SD_MOUSE_WHEEL;
    // UWP keeps the last relative delta in this context across button events.
    return sink_.Emit(protocol::MouseHeader{delta_x_, delta_y_, wheel ? wheel_ : 0, flags, 0});
}
sd_result SAMouseOutput::OnPointerMoved(const sd_pointer_sample& s, uint64_t) {
    if (s.update_kind >= 1 && s.update_kind <= 6) changes_ = 1u << (s.update_kind - 1);
    return Emit();
}
sd_result SAMouseOutput::OnPointerPressed(const sd_pointer_sample& s, uint64_t) {
    if (s.buttons & SD_POINTER_BUTTON_LEFT) changes_ = 1;
    else if (s.buttons & SD_POINTER_BUTTON_MIDDLE) changes_ = 16;
    else if (s.buttons & SD_POINTER_BUTTON_RIGHT) changes_ = 4;
    return Emit();
}
sd_result SAMouseOutput::OnPointerReleased(const sd_pointer_sample& s, uint64_t) {
    if (s.update_kind == 2 || s.update_kind == 4 || s.update_kind == 6) changes_ = 1u << (s.update_kind - 1);
    return Emit();
}
sd_result SAMouseOutput::OnPointerWheelChanged(const sd_pointer_sample& s, uint64_t) {
    changes_ = 0x400; wheel_ = s.wheel_delta; return Emit();
}
sd_result SAMouseOutput::OnRawMouseMoved(const sd_pointer_sample& s) {
    changes_ = 0; delta_x_ = s.delta_x; delta_y_ = s.delta_y; return Emit();
}
void SAPenOutput::Update(const sd_pointer_sample& s) {
    if (!s.mapped) return; // Original sends retained context when mapping fails.
    width_ = s.surface_width; height_ = s.surface_height;
    x_ = s.x; y_ = s.y; x_tilt_ = s.x_tilt; y_tilt_ = s.y_tilt;
    pressure_ = s.pressure; in_range_ = s.in_range != 0;
}
sd_result SAPenOutput::Emit() {
    protocol::PenHeader packet{};
    packet.surface_width = width_; packet.surface_height = height_;
    packet.x = x_; packet.y = y_; packet.pressure = pressure_;
    packet.contact = uint8_t(contact_); packet.in_range = uint8_t(in_range_);
    // The recovered FSM builder does not copy the retained tilt fields to wire.
    return sink_.Emit(packet);
}
sd_result SAPenOutput::OnPointerMoved(const sd_pointer_sample& s, uint64_t) { Update(s); return Emit(); }
sd_result SAPenOutput::OnPointerPressed(const sd_pointer_sample& s, uint64_t) {
    if (!s.capture_succeeded) return SD_IGNORED;
    Update(s); contact_ = true; return Emit();
}
sd_result SAPenOutput::OnPointerReleased(const sd_pointer_sample& s, uint64_t) {
    Update(s); contact_ = false; return Emit();
}
void SAPenOutput::Reset() noexcept {
    width_ = height_ = 0; x_ = y_ = x_tilt_ = y_tilt_ = pressure_ = 0;
    contact_ = in_range_ = false;
}
SATouchOutput::~SATouchOutput() { try { Reset(); } catch (...) {} }
void SATouchOutput::Store(const sd_pointer_sample& s) {
    if (!contacts_.count(s.pointer_id) && contacts_.size() >= protocol::max_additional_contacts + 1)
        throw Failure(SD_INVALID_ARGUMENT, "Touch output supports at most seven contacts");
    contacts_[s.pointer_id] = {s.x, s.y};
    width_ = s.surface_width; height_ = s.surface_height;
}
sd_result SATouchOutput::Emit(uint32_t flags) {
    snapshot_.clear();
    for (const auto& [id, point] : contacts_)
        snapshot_[id] = {int32_t(point.x), int32_t(point.y), uint16_t(flags), uint16_t(id)};
    protocol::TouchHeader packet{};
    packet.surface_width = width_; packet.surface_height = height_;
    packet.flags = uint16_t(flags | uint32_t(mode_));
    packet.timestamp_ms = uint32_t(std::clock());
    size_t index = 0;
    for (const auto& [id, point] : snapshot_) {
        if (!index) {
            packet.x = point.x; packet.y = point.y; packet.contact_id = uint16_t(id);
        } else {
            packet.additional[index - 1] = point;
            ++packet.additional_count;
        }
        ++index;
    }
    return sink_.Emit(packet);
}
sd_result SATouchOutput::Start(uint64_t now) {
    active_ = true;
    const auto result = Emit(2);
    next_tick_ = now + timer_interval_ms;
    return result;
}
sd_result SATouchOutput::Stop() { active_ = false; next_tick_ = 0; return Emit(4); }
sd_result SATouchOutput::RemoveOutside(uint32_t id) {
    if (!contacts_.count(id)) return SD_IGNORED;
    sd_result result = SD_OK;
    if (contacts_.size() == 1 && active_) result = Stop(); // UP includes the last position before erase.
    contacts_.erase(id);
    return result;
}
sd_result SATouchOutput::OnPointerMoved(const sd_pointer_sample& s, uint64_t now) {
    if (s.buttons & SD_POINTER_BUTTON_LEFT) {
        if (!s.mapped) return RemoveOutside(s.pointer_id);
        Store(s);
        if (!active_) return Start(now);
    } else if (active_) return Stop();
    return SD_OK;
}
sd_result SATouchOutput::OnPointerPressed(const sd_pointer_sample& s, uint64_t now) {
    if (!s.capture_succeeded || !(s.buttons & SD_POINTER_BUTTON_LEFT) || !s.mapped) return SD_IGNORED;
    Store(s);
    // Native mouse-to-touch starts immediately. Physical touch starts on move.
    if (s.device_type == SD_POINTER_MOUSE && !active_) return Start(now);
    return SD_OK;
}
sd_result SATouchOutput::OnPointerReleased(const sd_pointer_sample& s, uint64_t now) {
    if (s.update_kind != SD_POINTER_LEFT_RELEASED) return SD_IGNORED;
    if (!s.mapped) return RemoveOutside(s.pointer_id);
    width_ = s.surface_width; height_ = s.surface_height;
    sd_result result = SD_OK;
    if (contacts_.size() == 1) {
        if (active_) result = Stop();
        contacts_.erase(s.pointer_id);
    } else if (contacts_.size() > 1) {
        contacts_.erase(s.pointer_id);
        if (!active_) result = Start(now);
    }
    return result;
}
sd_result SATouchOutput::SetMode(bool mode) {
    if (active_ || !contacts_.empty()) return SD_INVALID_ARGUMENT;
    mode_ = mode; return SD_OK;
}
sd_result SATouchOutput::Reset(bool release) {
    const bool send_up = active_ && release;
    active_ = false; next_tick_ = 0;
    sd_result result = SD_OK;
    try { if (send_up) result = Emit(4); }
    catch (...) { contacts_.clear(); snapshot_.clear(); width_ = height_ = 0; throw; }
    contacts_.clear(); snapshot_.clear(); width_ = height_ = 0; next_tick_ = 0;
    return result;
}
void SATouchOutput::ProcessTick(uint64_t now) {
    if (active_ && now >= next_tick_) {
        Emit(mode_ ? 1 : 2);
        // DispatcherTimer coalesces delayed ticks instead of replaying a burst.
        next_tick_ = now + timer_interval_ms;
    }
}
uint32_t SATouchOutput::UntilTick(uint64_t now) const noexcept {
    if (!active_) return std::numeric_limits<uint32_t>::max();
    return now >= next_tick_ ? 0 : uint32_t(next_tick_ - now);
}
}
