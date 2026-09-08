#pragma once
#include "spacedesk_sdk.h"
#include "spacedesk_protocol.hpp"
#include <array>

namespace sd::native {
// SDK role alias: APK S1 has no recovered original class name or explicit base.
// Retains its false-initialized CapsLock bit and Activity X0/y0 key ordering.
class APKKeyboardInput {
    bool caps_lock_=false;
public:
    struct Packets {
        std::array<protocol::KeyboardHeader,2> values{};
        uint32_t count=0;
    };
    void Reset() noexcept { caps_lock_=false; }
    Packets Process(const sd_android_key_event& event) noexcept {
        Packets result;
        if(event.action==SD_ANDROID_KEY_MULTIPLE) return result;
        constexpr uint32_t caps_key=115; // S1.d singleton: Android KEYCODE_CAPS_LOCK.
        if(event.key_code==caps_key) {
            if(event.action==SD_ANDROID_KEY_UP) {
                caps_lock_=!caps_lock_;
                result.values[result.count++]={caps_key,0,SD_KEY_PRESS};
            }
            return result; // APK omits CapsLock DOWN.
        }
        if(event.action==SD_ANDROID_KEY_DOWN && caps_lock_!=(event.caps_lock_on!=0)) {
            result.values[result.count++]={caps_key,0,SD_KEY_PRESS};
            caps_lock_=event.caps_lock_on!=0;
        }
        result.values[result.count++]={event.key_code,0,event.action==SD_ANDROID_KEY_UP?SD_KEY_UP:0u};
        return result;
    }
};
}
