#pragma once
#include "spacedesk_sdk.h"

namespace sd {
inline bool ValidMotionEvent(const sd_motion_event* input,bool require_action_index=true) noexcept {
    if(!input || input->struct_size!=sizeof(*input) || input->reserved ||
       !input->pointer_count || input->pointer_count>SD_MOTION_MAX_POINTERS ||
       (require_action_index && ((input->action&0xff00)>>8)>=input->pointer_count)) return false;
    uint32_t ids=0;
    for(uint32_t i=0;i<input->pointer_count;++i) {
        const auto id=input->pointers[i].pointer_id;
        if(id<0 || id>31 || (ids&(1u<<id))) return false;
        ids|=1u<<id;
    }
    return true;
}
}
