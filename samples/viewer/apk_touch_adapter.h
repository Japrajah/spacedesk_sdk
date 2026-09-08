#pragma once
#include <windows.h>
#include "spacedesk_sdk.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace sd::viewer {
// Win32 frontend boundary for APK MotionEvent batches. It retains contacts in
// pointer-index order and never interprets taps/scrolling as mouse packets.
class APKTouchAdapter {
    struct Pointer { sd_pointer_sample current{}, mapped{}; };
    std::array<Pointer, 7> pointers_{};
    uint32_t count_ = 0, width_ = 0, height_ = 0;
    uint32_t Find(uint32_t id) const noexcept {
        for (uint32_t index = 0; index < count_; ++index)
            if (pointers_[index].current.pointer_id == id) return index;
        return count_;
    }
    static int32_t JavaInt(float value) noexcept {
        // Matches Java's floating-point to int conversion without C++ UB.
        if (std::isnan(value)) return 0;
        if (value >= double(std::numeric_limits<int32_t>::max())) return std::numeric_limits<int32_t>::max();
        if (value <= double(std::numeric_limits<int32_t>::min())) return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(value);
    }
    void Store(uint32_t index, const sd_pointer_sample& sample) noexcept {
        pointers_[index].current = sample;
        if (sample.mapped) pointers_[index].mapped = sample;
        if (sample.surface_width && sample.surface_height) {
            width_ = sample.surface_width; height_ = sample.surface_height;
        }
    }
    sd_touch_input Batch(uint32_t action, uint64_t now, bool release = false) const noexcept {
        sd_touch_input input{}; input.struct_size = sizeof(input);
        input.action = action; input.surface_width = width_; input.surface_height = height_;
        input.timestamp_ms = uint32_t(now); input.contact_count = count_;
        for (uint32_t index = 0; index < count_; ++index) {
            const auto& pointer = pointers_[index];
            // Win32 focus/capture loss has no fresh mapped coordinates. A final
            // release uses the last valid sample so a pointer leaving the image
            // cannot stay down remotely. This cleanup is a frontend guarantee;
            // the pure APK builder still preserves Android mapping-failure skips.
            const auto& sample = release && !pointer.current.mapped && pointer.mapped.mapped ?
                pointer.mapped : pointer.current;
            input.contacts[index] = {sample.pointer_id, JavaInt(sample.x), JavaInt(sample.y), sample.mapped ? 1u : 0u};
        }
        return input;
    }
    void Clear() noexcept { pointers_ = {}; count_ = width_ = height_ = 0; }
public:
    bool Tracked(uint32_t id) const noexcept { return Find(id) < count_; }
    uint32_t Count() const noexcept { return count_; }
    template<class Send> sd_result Reset(uint64_t wallclock_ms, Send&& send) {
        if (!count_) return SD_IGNORED;
        const auto input = Batch(3, wallclock_ms, true); // Android ACTION_CANCEL
        Clear(); return send(input);
    }
    template<class Send> sd_result CaptureLost(uint32_t id, uint64_t wallclock_ms, Send&& send) {
        // Android CANCEL terminates the whole gesture, not an index-specific UP.
        return Tracked(id) ? Reset(wallclock_ms, send) : SD_IGNORED;
    }
    template<class Send> sd_result Process(UINT message, const sd_pointer_sample& sample,
                                           bool in_contact, bool canceled, bool enabled,
                                           uint64_t wallclock_ms, Send&& send) {
        if (sample.device_type != SD_POINTER_TOUCH) return SD_IGNORED;
        auto index = Find(sample.pointer_id);
        if (canceled || !enabled || message == WM_POINTERCAPTURECHANGED || message == WM_POINTERLEAVE)
            return index < count_ ? Reset(wallclock_ms, send) : SD_IGNORED;
        if (message == WM_POINTERDOWN) {
            if (index < count_ || count_ == pointers_.size() || !in_contact || !sample.capture_succeeded ||
                !sample.surface_width || !sample.surface_height) return SD_IGNORED;
            // Even an outside contact remains in the OS batch, so a later valid
            // secondary pointer reproduces SAMotionEvent's fallback selection.
            index = count_++; Store(index, sample);
            return send(Batch(index == 0 ? 0u : (5u | (index << 8)), wallclock_ms));
        }
        if (index == count_) return SD_IGNORED;
        if (message == WM_POINTERUP) {
            Store(index, sample);
            const auto input = Batch(count_ == 1 ? 1u : (6u | (index << 8)), wallclock_ms, count_ == 1);
            for (uint32_t next = index + 1; next < count_; ++next) pointers_[next - 1] = pointers_[next];
            pointers_[--count_] = {};
            if (!count_) width_ = height_ = 0;
            return send(input);
        }
        if (message != WM_POINTERUPDATE) return SD_IGNORED;
        if (!in_contact) return Reset(wallclock_ms, send);
        Store(index, sample);
        return send(Batch(2, wallclock_ms));
    }
};
}
