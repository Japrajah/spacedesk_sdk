#pragma once
#include <windows.h>
#include "spacedesk_sdk.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace sd::viewer {
// Win32 -> SDK adapter only. The DLL remains responsible for SAPenOutput and
// SATouchOutput state, their packet layout, and the native 25 ms touch timer.
inline bool PromotedMouse(ULONG_PTR extra) noexcept {
    return (extra & 0xffffff00u) == 0xff515700u;
}

inline uint32_t UpdateKind(POINTER_BUTTON_CHANGE_TYPE change) noexcept {
    switch (change) {
    case POINTER_CHANGE_FIRSTBUTTON_DOWN: return SD_POINTER_LEFT_PRESSED;
    case POINTER_CHANGE_FIRSTBUTTON_UP: return SD_POINTER_LEFT_RELEASED;
    case POINTER_CHANGE_SECONDBUTTON_DOWN: return SD_POINTER_RIGHT_PRESSED;
    case POINTER_CHANGE_SECONDBUTTON_UP: return SD_POINTER_RIGHT_RELEASED;
    case POINTER_CHANGE_THIRDBUTTON_DOWN: return SD_POINTER_MIDDLE_PRESSED;
    case POINTER_CHANGE_THIRDBUTTON_UP: return SD_POINTER_MIDDLE_RELEASED;
    default: return SD_POINTER_UPDATE_OTHER;
    }
}

inline sd_pointer_sample Sample(const POINTER_INFO& info, POINT client_position,
                               const POINTER_PEN_INFO* pen = nullptr) noexcept {
    sd_pointer_sample result{};
    result.struct_size = sizeof(result);
    result.device_type = info.pointerType == PT_PEN ? SD_POINTER_PEN : SD_POINTER_TOUCH;
    result.pointer_id = info.pointerId;
    result.update_kind = UpdateKind(info.ButtonChangeType);
    result.x = float(client_position.x); result.y = float(client_position.y);
    result.in_range = (info.pointerFlags & POINTER_FLAG_INRANGE) != 0;
    if (info.pointerFlags & POINTER_FLAG_FIRSTBUTTON) result.buttons |= SD_POINTER_BUTTON_LEFT;
    if (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) result.buttons |= SD_POINTER_BUTTON_RIGHT;
    if (info.pointerFlags & POINTER_FLAG_THIRDBUTTON) result.buttons |= SD_POINTER_BUTTON_MIDDLE;
    // Touch contact is the SDK's left-button property even on devices that do
    // not explicitly provide POINTER_FLAG_FIRSTBUTTON.
    if (info.pointerType == PT_TOUCH && (info.pointerFlags & POINTER_FLAG_INCONTACT))
        result.buttons |= SD_POINTER_BUTTON_LEFT;
    if (pen) {
        if (pen->penMask & PEN_MASK_PRESSURE) result.pressure = float(std::min(pen->pressure, 1024u)) / 1024.0f;
        if (pen->penMask & PEN_MASK_TILT_X) result.x_tilt = float(pen->tiltX);
        if (pen->penMask & PEN_MASK_TILT_Y) result.y_tilt = float(pen->tiltY);
    }
    return result;
}

inline void ToDips(sd_pointer_sample& sample, UINT dpi) noexcept {
    const float scale = 96.0f / (dpi ? dpi : 96);
    sample.x *= scale; sample.y *= scale;
}

// This is the exact rectangle passed to StretchDIBits by the GDI viewer. GPU
// samples instead use ToDips followed by sd_client_map_pointer_sample.
inline void MapGdi(sd_pointer_sample& sample, const RECT& image,
                   uint32_t width, uint32_t height) noexcept {
    sample.surface_width = width; sample.surface_height = height; sample.mapped = 0;
    if (!width || !height || image.right <= image.left || image.bottom <= image.top ||
        !std::isfinite(sample.x) || !std::isfinite(sample.y) ||
        sample.x < image.left || sample.x >= image.right ||
        sample.y < image.top || sample.y >= image.bottom) return;
    sample.x = (sample.x - image.left) * float(width) / float(image.right - image.left);
    sample.y = (sample.y - image.top) * float(height) / float(image.bottom - image.top);
    sample.mapped = 1;
}

class PointerAdapter {
    struct Pointer {
        bool used = false, contact = false;
        sd_pointer_sample current{}, mapped{};
    };
    // SATouchOutput wire capacity is seven contacts; SAPenOutput holds one pen.
    std::array<Pointer, 7> touch_{};
    Pointer pen_{};

    Pointer* Find(uint32_t type, uint32_t id) noexcept {
        if (type == SD_POINTER_PEN) return pen_.used && pen_.current.pointer_id == id ? &pen_ : nullptr;
        for (auto& p : touch_) if (p.used && p.current.pointer_id == id) return &p;
        return nullptr;
    }
    Pointer* Allocate(uint32_t type) noexcept {
        if (type == SD_POINTER_PEN) return pen_.used ? nullptr : &pen_;
        for (auto& p : touch_) if (!p.used) return &p;
        return nullptr;
    }
    static void Store(Pointer& pointer, const sd_pointer_sample& sample) noexcept {
        pointer.used = true; pointer.current = sample;
        if (sample.mapped) pointer.mapped = sample;
    }
    template<class Send> static sd_result Release(Pointer& pointer, Send&& send) {
        if (!pointer.used) return SD_IGNORED;
        // Capture-change has no new pointer data. Pen uses its last mapped
        // position to clear in-range. Touch must retain a preceding mapping
        // miss: that contact was already removed from the native map, so its
        // late release must not release a different remaining contact.
        auto sample = pointer.current.device_type == SD_POINTER_PEN ? pointer.mapped : pointer.current;
        sample.buttons = 0; sample.pressure = 0; sample.in_range = 0;
        sample.update_kind = SD_POINTER_LEFT_RELEASED; sample.capture_succeeded = 0;
        const auto type = pointer.current.device_type;
        pointer = {}; // Clear before callback to make error cleanup reentrant.
        return send(type, SD_POINTER_CAPTURE_LOST, sample);
    }

public:
    bool Tracked(uint32_t id) const noexcept {
        if (pen_.used && pen_.current.pointer_id == id) return true;
        for (const auto& p : touch_) if (p.used && p.current.pointer_id == id) return true;
        return false;
    }

    template<class Send> sd_result CaptureLost(uint32_t id, Send&& send) {
        if (pen_.used && pen_.current.pointer_id == id) return Release(pen_, send);
        for (auto& p : touch_) if (p.used && p.current.pointer_id == id) return Release(p, send);
        return SD_IGNORED;
    }

    // All client-area messages for touch/pen are consumed by the window even
    // when input is disabled or no frame is available; DefWindowProc must not
    // promote the unhandled part of a gesture into mouse clicks.
    template<class Send> sd_result Process(UINT message, sd_pointer_sample sample,
                                           bool in_contact, bool canceled, bool enabled, Send&& send) {
        if (sample.device_type != SD_POINTER_TOUCH && sample.device_type != SD_POINTER_PEN) return SD_IGNORED;
        auto* pointer = Find(sample.device_type, sample.pointer_id);
        if (canceled || message == WM_POINTERCAPTURECHANGED || message == WM_POINTERLEAVE || !enabled) {
            return pointer ? Release(*pointer, send) : SD_IGNORED;
        }
        if (sample.pointer_id > 65535) return SD_IGNORED;
        if (message == WM_POINTERDOWN) {
            if (!sample.mapped || !sample.capture_succeeded || !in_contact) return SD_IGNORED;
            // A duplicate DOWN must not overwrite another pen/contact's state.
            if (pointer && pointer->contact) return SD_IGNORED;
            if (!pointer) pointer = Allocate(sample.device_type);
            if (!pointer) return SD_IGNORED;
            sample.update_kind = SD_POINTER_LEFT_PRESSED;
            Store(*pointer, sample); pointer->contact = true;
            return send(sample.device_type, SD_POINTER_PRESSED, sample);
        }
        if (message == WM_POINTERUP) {
            if (!pointer || !pointer->contact) return SD_IGNORED;
            sample.update_kind = SD_POINTER_LEFT_RELEASED;
            sample.buttons &= ~SD_POINTER_BUTTON_LEFT;
            if (sample.device_type == SD_POINTER_TOUCH) {
                if (!pointer->current.mapped) sample.mapped = 0;
                *pointer = {};
            }
            else { Store(*pointer, sample); pointer->contact = false; }
            return send(sample.device_type, SD_POINTER_RELEASED, sample);
        }
        if (message != WM_POINTERUPDATE && message != WM_POINTERENTER) return SD_IGNORED;
        if (message == WM_POINTERENTER && sample.device_type == SD_POINTER_TOUCH) return SD_IGNORED;
        if (!pointer) {
            // A contact that began elsewhere (or before F8/focus loss) cannot
            // re-enter as a new remote DOWN. An uncaptured pen hover is valid.
            if (sample.device_type != SD_POINTER_PEN || in_contact || !sample.in_range || !sample.mapped)
                return SD_IGNORED;
            pointer = Allocate(sample.device_type);
            if (!pointer) return SD_IGNORED;
        }
        if (!pointer->contact && in_contact) return SD_IGNORED;
        if (pointer->contact && !in_contact) return Release(*pointer, send);
        Store(*pointer, sample);
        return send(sample.device_type, SD_POINTER_MOVED, sample);
    }

    template<class Send> void Reset(Send&& send) {
        Release(pen_, send);
        for (auto& pointer : touch_) Release(pointer, send);
    }
};
}
