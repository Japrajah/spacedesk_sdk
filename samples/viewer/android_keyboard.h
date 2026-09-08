#pragma once
#include <windows.h>
#include <cstdint>
#include <optional>

namespace sd::viewer {
// Win32 frontend adapter for a profile declaring Android OS3. Android keycodes
// are a separate namespace, not Win32 VK numbers. Unknown mappings are omitted.
inline std::optional<uint32_t> AndroidKey(uint32_t key,uint32_t scan,bool extended) noexcept {
    if(key>='A' && key<='Z') return 29+key-'A';
    if(key>='0' && key<='9') return 7+key-'0';
    if(key>=VK_F1 && key<=VK_F12) return 131+key-VK_F1;
    if(key>=VK_F13 && key<=VK_F24) return 326+key-VK_F13; // Android API36 keycodes.
    if(key>=VK_NUMPAD0 && key<=VK_NUMPAD9) return 144+key-VK_NUMPAD0;
    switch(key) {
    case VK_SHIFT:return scan==0x36?60:59;
    case VK_CONTROL:return extended?114:113;
    case VK_MENU:return extended?58:57;
    case VK_LSHIFT:return 59;case VK_RSHIFT:return 60;
    case VK_LCONTROL:return 113;case VK_RCONTROL:return 114;
    case VK_LMENU:return 57;case VK_RMENU:return 58;
    case VK_LWIN:return 117;case VK_RWIN:return 118;
    case VK_CAPITAL:return 115;case VK_SPACE:return 62;
    case VK_BACK:return 67;case VK_RETURN:return extended?160:66;
    case VK_TAB:return 61;case VK_ESCAPE:return 111;
    case VK_LEFT:return 21;case VK_UP:return 19;case VK_RIGHT:return 22;case VK_DOWN:return 20;
    case VK_PRIOR:return 92;case VK_NEXT:return 93;
    case VK_HOME:return 122;case VK_END:return 123;case VK_INSERT:return 124;case VK_DELETE:return 112;
    case VK_APPS:return 82;case VK_NUMLOCK:return 143;case VK_SCROLL:return 116;
    case VK_PAUSE:return 121;case VK_SNAPSHOT:return 120;
    case VK_DIVIDE:return 154;case VK_MULTIPLY:return 155;case VK_SUBTRACT:return 156;
    case VK_ADD:return 157;case VK_DECIMAL:return 158;case VK_SEPARATOR:return 159;
    case VK_OEM_1:return 74;case VK_OEM_PLUS:return 70;case VK_OEM_COMMA:return 55;
    case VK_OEM_MINUS:return 69;case VK_OEM_PERIOD:return 56;case VK_OEM_2:return 76;
    case VK_OEM_3:return 68;case VK_OEM_4:return 71;case VK_OEM_5:return 73;
    case VK_OEM_6:return 72;case VK_OEM_7:return 75;
    case VK_MEDIA_NEXT_TRACK:return 87;case VK_MEDIA_PREV_TRACK:return 88;
    case VK_MEDIA_STOP:return 86;case VK_MEDIA_PLAY_PAUSE:return 85;
    case VK_VOLUME_UP:return 24;case VK_VOLUME_DOWN:return 25;case VK_VOLUME_MUTE:return 164;
    default:return std::nullopt;
    }
}
}
