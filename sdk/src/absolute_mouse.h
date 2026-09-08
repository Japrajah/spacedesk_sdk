#pragma once
#include "frame_decoder.h"
#include "spacedesk_protocol.hpp"
#include <algorithm>
#include <cmath>

namespace sd {
inline void ValidateMouseTarget(const sd_mouse_target& t) {
    if(t.struct_size!=sizeof(t) || t.rotation<1 || t.rotation>4)
        throw Failure(SD_INVALID_ARGUMENT,"Invalid absolute mouse target");
    const bool primary=!t.desktop_width&&!t.desktop_height&&!t.display_width&&!t.display_height;
    if(primary) {
        if(t.desktop_left||t.desktop_top||t.display_left||t.display_top)
            throw Failure(SD_INVALID_ARGUMENT,"Primary mouse target must have empty rectangles");
        return;
    }
    if(!t.desktop_width||!t.desktop_height||!t.display_width||!t.display_height||
       t.desktop_width>INT32_MAX||t.desktop_height>INT32_MAX||t.display_width>INT32_MAX||t.display_height>INT32_MAX||
       t.display_left<t.desktop_left||t.display_top<t.desktop_top||
       int64_t(t.display_left)+t.display_width>int64_t(t.desktop_left)+t.desktop_width||
       int64_t(t.display_top)+t.display_height>int64_t(t.desktop_top)+t.desktop_height)
        throw Failure(SD_INVALID_ARGUMENT,"Mouse display rectangle must be inside the remote desktop rectangle");
}
inline protocol::MouseHeader AbsoluteMouse(const sd_mouse_target& target,float x,float y,
    uint32_t width,uint32_t height,int32_t data,uint32_t buttons) {
    if(!width||!height||width>INT32_MAX||height>INT32_MAX||!std::isfinite(x)||!std::isfinite(y)||
       (buttons&~uint32_t(0x187f))) throw Failure(SD_INVALID_ARGUMENT,"Invalid absolute mouse position or buttons");
    double u=(std::clamp(double(x),0.0,double(width-1))+.5)/width;
    double v=(std::clamp(double(y),0.0,double(height-1))+.5)/height;
    const auto old_u=u;
    switch(target.rotation) {
    case 2:u=1-v;v=old_u;break;
    case 3:u=1-u;v=1-v;break;
    case 4:u=v;v=1-old_u;break;
    }
    uint32_t flags=SD_MOUSE_COORD_ABSOLUTE;
    if(target.desktop_width) {
        u=(double(target.display_left)-target.desktop_left+u*target.display_width)/target.desktop_width;
        v=(double(target.display_top)-target.desktop_top+v*target.display_height)/target.desktop_height;
        flags|=SD_MOUSE_COORD_VIRTUAL_DESKTOP;
    }
    // Mouse-class normalized coordinates; +24 is copied to MOUSE_INPUT_DATA.Flags.
    return {int32_t(std::clamp(std::floor(u*65536.0),0.0,65535.0)),
        int32_t(std::clamp(std::floor(v*65536.0),0.0,65535.0)),data,buttons|SD_MOUSE_MOVE,flags};
}
}
