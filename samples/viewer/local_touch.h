#pragma once
#include <windows.h>
#include "spacedesk_sdk.h"

namespace sd::viewer {
// Classic MouseTouchEnable cursor/capture ownership. No raw-input registration
// or cursor hiding: local mode retains ordinary WM_MOUSE messages and a cross.
class LocalTouchCapture {
    HWND window_=nullptr;
    RECT previous_clip_{},owned_clip_{};
    HCURSOR previous_cursor_=nullptr;
    bool saved_clip_=false;
public:
    bool Active() const noexcept { return window_!=nullptr; }
    void End() noexcept {
        const HWND window=window_;window_=nullptr;
        if(!window) return;
        SetCursor(previous_cursor_);
        if(GetCapture()==window) ReleaseCapture();
        RECT current{};
        if(saved_clip_ && GetClipCursor(&current) && EqualRect(&current,&owned_clip_)) ClipCursor(&previous_clip_);
        saved_clip_=false;previous_cursor_=nullptr;
    }
    bool UpdateBounds() noexcept {
        if(!window_) return false;
        RECT area{};
        if(IsIconic(window_) || !GetClientRect(window_,&area) || area.right<=0 || area.bottom<=0) { End();return false; }
        MapWindowPoints(window_,nullptr,reinterpret_cast<POINT*>(&area),2);
        if(!ClipCursor(&area)) { End();return false; }
        owned_clip_=area;return true;
    }
    bool Begin(HWND window) noexcept {
        if(window_==window) return true;
        End();window_=window;previous_cursor_=GetCursor();saved_clip_=GetClipCursor(&previous_clip_)!=FALSE;
        if(!saved_clip_ || !UpdateBounds()) { End();return false; }
        if(GetCapture()!=window) SetCapture(window);
        if(GetCapture()!=window) { End();return false; }
        SetCursor(LoadCursorW(nullptr,IDC_CROSS));return true;
    }
    ~LocalTouchCapture() { End(); }
};

// Classic immediate WM input behavior over the SDK's typed raw touch API.
// The outer MouseToTouchAdapter normalizes physical LMB/RMB to one finger.
class ClassicTouchOutput {
    bool contact_=false;
    sd_pointer_sample last_{};
public:
    template<class Send> sd_result Process(uint32_t action,const sd_pointer_sample& sample,uint32_t timestamp,Send&& send) {
        uint16_t flags=0;
        const bool held=sample.mapped && (sample.buttons&SD_POINTER_BUTTON_LEFT);
        if((action==SD_POINTER_PRESSED || action==SD_POINTER_MOVED) && held) {
            flags=contact_?1:3;contact_=true;last_=sample;
        } else {
            if(!contact_) return SD_IGNORED;
            contact_=false;flags=5;
            // Classic UP uses its own coordinates. Capture loss/outside events
            // without a valid point use the last mapped position for cleanup.
            if(sample.mapped) last_=sample;
        }
        sd_touch_event event{};event.struct_size=sizeof(event);event.contact_count=1;
        event.surface_width=last_.surface_width;event.surface_height=last_.surface_height;
        event.timestamp_ms=timestamp;event.contacts[0]={int32_t(last_.x),int32_t(last_.y),flags,0};
        return send(event);
    }
};
}
