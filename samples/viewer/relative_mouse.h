#pragma once
#include <windows.h>

namespace sd::viewer {
// Windows UI ownership only: raw registration, capture, confinement and cursor
// visibility never enter the SDK. Mirrors the classic viewer's NOLEGACY mode.
class RelativeMouseCapture {
    HWND window_=nullptr;
    RECT previous_clip_{},owned_clip_{};
    int cursor_balance_=0;
    bool clip_saved_=false;
public:
    bool Active() const noexcept { return window_!=nullptr; }
    void End() noexcept {
        const auto window=window_;window_=nullptr;
        if(!window) return;
        const RAWINPUTDEVICE device{1,2,0,window};
        RegisterRawInputDevices(&device,1,sizeof(device));
        if(GetCapture()==window) ReleaseCapture();
        RECT current{};
        if(clip_saved_ && GetClipCursor(&current) && EqualRect(&current,&owned_clip_)) ClipCursor(&previous_clip_);
        clip_saved_=false;
        while(cursor_balance_>0) { ShowCursor(TRUE);--cursor_balance_; }
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));
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
        End();
        const RAWINPUTDEVICE device{1,2,RIDEV_NOLEGACY|RIDEV_CAPTUREMOUSE,window};
        if(!RegisterRawInputDevices(&device,1,sizeof(device))) return false;
        window_=window;clip_saved_=GetClipCursor(&previous_clip_)!=0;
        if(!clip_saved_) { End();return false; }
        if(!UpdateBounds()) return false;
        SetCapture(window);
        if(GetCapture()!=window) { End();return false; }
        do { ++cursor_balance_; } while(ShowCursor(FALSE)>=0 && cursor_balance_<32);
        SetCursor(nullptr);return true;
    }
    ~RelativeMouseCapture() { End(); }
};
}
