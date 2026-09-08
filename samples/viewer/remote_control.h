#pragma once
#include <windows.h>
#include "spacedesk_sdk.h"
#include <cstdint>
#include <cwctype>

namespace sd::viewer {
// Frontend adapter for SARemoteControl mouse-output mode 1. The SDK keeps its
// native SATouchOutput target; the Win32 viewer selects classic immediate output.
class MouseToTouchAdapter {
    bool tracked_ = false;
    sd_pointer_sample current_{};
    static sd_pointer_sample Mouse(sd_pointer_sample sample) noexcept {
        sample.device_type = SD_POINTER_MOUSE;
        sample.pointer_id = 0;
        return sample;
    }
public:
    bool Active() const noexcept { return tracked_; }
    template<class Send> sd_result Reset(Send&& send) {
        if (!tracked_) return SD_IGNORED;
        auto sample = current_;
        sample.buttons = 0; sample.update_kind = SD_POINTER_LEFT_RELEASED;
        sample.capture_succeeded = 0;
        tracked_ = false; current_ = {};
        return send(SD_POINTER_TOUCH, SD_POINTER_CAPTURE_LOST, sample);
    }
    template<class Send> sd_result CaptureLost(Send&& send) { return Reset(send); }
    // `enabled` combines the physical mouse master gate, output-mode selection
    // and touch-packet capability. Frontend capture is independent of pen/touch.
    template<class Send> sd_result Process(uint32_t event, sd_pointer_sample sample,
                                           bool enabled, Send&& send) {
        if (!enabled || event == SD_POINTER_CAPTURE_LOST) return Reset(send);
        sample = Mouse(sample);
        if (event == SD_POINTER_PRESSED) {
            if (tracked_ || !sample.capture_succeeded || !sample.mapped ||
                !(sample.buttons & SD_POINTER_BUTTON_LEFT) ||
                (sample.update_kind != SD_POINTER_UPDATE_OTHER && sample.update_kind != SD_POINTER_LEFT_PRESSED))
                return SD_IGNORED;
            sample.update_kind = SD_POINTER_LEFT_PRESSED;
            current_ = sample; tracked_ = true;
            return send(SD_POINTER_TOUCH, SD_POINTER_PRESSED, sample);
        }
        if (!tracked_) return SD_IGNORED;
        if (event == SD_POINTER_RELEASED) {
            if (sample.update_kind != SD_POINTER_LEFT_RELEASED) return SD_IGNORED;
            // A preceding mapped=0 move already removed this native contact.
            // Keep the missing-contact branch if release happens to map again.
            if (!current_.mapped) sample.mapped = 0;
            sample.buttons &= ~SD_POINTER_BUTTON_LEFT;
            current_ = {}; tracked_ = false;
            return send(SD_POINTER_TOUCH, SD_POINTER_RELEASED, sample);
        }
        if (event != SD_POINTER_MOVED) return SD_IGNORED; // No wheel/raw mouse emulation.
        current_ = sample;
        return send(SD_POINTER_TOUCH, SD_POINTER_MOVED, sample);
    }
};

// Portable frontend reconstruction of the recovered exit-combination matcher.
// A successful chord ends on release of ALL selected keys, never on key-down.
class RemoteExitChord {
    // Three source bits per modifier: generic, left, right; one for the letter.
    uint16_t down_ = 0;
    uint8_t selected_ = 2 | 4; // Native defaults: Alt + Shift, no letter/Ctrl.
    wchar_t letter_ = 0;
    uint8_t phase_bits_ = 0, remaining_ = 2, state_ = 0;
    uint16_t Source(uint32_t key) const noexcept {
        if (selected_ & 1) {
            if (key == VK_CONTROL) return 1;
            if (key == VK_LCONTROL) return 2;
            if (key == VK_RCONTROL) return 4;
        }
        if (selected_ & 2) {
            if (key == VK_MENU) return 8;
            if (key == VK_LMENU) return 16;
            if (key == VK_RMENU) return 32;
        }
        if (selected_ & 4) {
            if (key == VK_SHIFT) return 64;
            if (key == VK_LSHIFT) return 128;
            if (key == VK_RSHIFT) return 256;
        }
        return (selected_ & 8) && key == uint32_t(letter_) ? 512 : 0;
    }
    uint8_t Held() const noexcept {
        return uint8_t((down_ & 7 ? 1 : 0) | (down_ & 56 ? 2 : 0) |
            (down_ & 448 ? 4 : 0) | (down_ & 512 ? 8 : 0));
    }
    uint8_t Count() const noexcept {
        return uint8_t((selected_ & 1 ? 1 : 0) + (selected_ & 2 ? 1 : 0) +
            (selected_ & 4 ? 1 : 0) + (selected_ & 8 ? 1 : 0));
    }
    void ResetMatcher() noexcept { phase_bits_ = 0; remaining_ = Count(); state_ = 0; }
    bool Match(uint8_t key, bool down) noexcept {
        switch (state_) {
        case 0: // Original state 0 counts selected keys not yet held.
            remaining_ = uint8_t(remaining_ + ((phase_bits_ & key) != 0) - unsigned(down));
            if (down) phase_bits_ |= key; else phase_bits_ &= uint8_t(~key);
            if (!remaining_) state_ = 1;
            return false;
        case 1:
            if (down) return false;
            // Exact native first-UP transition: no completion check here.
            // UWP settings validation requires at least two selected keys.
            state_ = 2; phase_bits_ = key; remaining_ = uint8_t(Count() - 1);
            return false;
        case 2: // The bitset now records released keys, not pressed keys.
            remaining_ = uint8_t(remaining_ + ((phase_bits_ & key) != 0) - unsigned(!down));
            if (down) phase_bits_ &= uint8_t(~key); else phase_bits_ |= key;
            if (!remaining_) { state_ = 3; return true; }
            return false;
        case 3:
            if (down) {
                // Original state 3 consumes the first new DOWN after reset;
                // it does not run state 0's completion check on that event.
                ResetMatcher(); phase_bits_ = key; remaining_ = uint8_t(Count() - 1);
            }
            return false;
        }
        return false;
    }
public:
    // GUI validates a one-letter field; the native matcher consumes its first
    // UTF-16 unit. Zero is the empty field. Invalid input leaves config intact.
    bool Configure(bool control, bool alt, bool shift, wchar_t letter = 0) noexcept {
        if (letter && !std::iswalpha(letter)) return false;
        letter_ = letter ? wchar_t(std::towupper(letter)) : 0;
        selected_ = uint8_t((control ? 1 : 0) | (alt ? 2 : 0) | (shift ? 4 : 0) | (letter_ ? 8 : 0));
        Reset(); return true;
    }
    void Reset() noexcept { down_ = 0; ResetMatcher(); }
    uint32_t State() const noexcept { return state_; }
    uint32_t Remaining() const noexcept { return remaining_; }
    bool OnKey(uint32_t key, bool down) noexcept {
        const auto source = Source(key);
        if (!source || !selected_) return false;
        if (down) down_ |= source;
        else down_ &= uint16_t(~source);
        const uint8_t group = source & 7 ? 1 : source & 56 ? 2 : source & 448 ? 4 : 8;
        if (!down && (Held() & group)) return false; // Another physical modifier side still held.
        return Match(group, down);
    }
};
}
