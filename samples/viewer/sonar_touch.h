#pragma once
#include <windows.h>
#include "spacedesk_sdk.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <optional>

namespace sd::viewer {
struct SonarTouchSample {
    uint32_t windows_id=0;
    sd_motion_point point{};
    bool mapped=false,captured=false,contact=false,canceled=false;
};
// Win32 has no Android-normalized AXIS_SIZE. A configured maximum contact
// diameter enables a linear Windows adapter; zero leaves size unavailable.
inline sd_motion_point SonarTouchMetrics(float x,float y,const POINTER_TOUCH_INFO* info,uint32_t maximum_diameter) {
    sd_motion_point point{};point.tool_type=1;point.axes[0]=x;point.axes[1]=y;point.axes[2]=.5f;
    if(info && (info->touchMask&TOUCH_MASK_PRESSURE) && info->pressure<=1024) point.axes[2]=float(info->pressure)/1024;
    if(info && (info->touchMask&TOUCH_MASK_CONTACTAREA)) {
        const double width=double(info->rcContact.right)-info->rcContact.left,height=double(info->rcContact.bottom)-info->rcContact.top;
        if(width>0 && height>0) {
            point.axes[4]=float(std::max(width,height));point.axes[5]=float(std::min(width,height));
            if(maximum_diameter) point.axes[3]=float(std::min(1.,std::sqrt(width*height)/maximum_diameter));
        }
    }
    return point;
}
class SonarTouchBatch {
    struct Contact { uint32_t windows_id=0;sd_motion_point point{},last_mapped{};bool have_mapped=false; };
    std::array<Contact,SD_MOTION_MAX_POINTERS> contacts_{};
    uint32_t count_=0;int64_t down_time_=0;
    uint32_t Find(uint32_t id) const noexcept { for(uint32_t i=0;i<count_;++i) if(contacts_[i].windows_id==id) return i;return count_; }
    void Store(uint32_t index,const SonarTouchSample& sample) {
        const auto id=contacts_[index].point.pointer_id;contacts_[index].point=sample.point;contacts_[index].point.pointer_id=id;
        if(sample.mapped) { contacts_[index].last_mapped=contacts_[index].point;contacts_[index].have_mapped=true; }
    }
    sd_motion_event Batch(uint32_t action,int64_t now,bool release=false) const {
        sd_motion_event event{};event.struct_size=sizeof(event);event.action=action;event.pointer_count=count_;
        event.down_time_ms=down_time_;event.event_time_ms=now;event.source=4098;event.x_precision=event.y_precision=1;
        for(uint32_t i=0;i<count_;++i) event.pointers[i]=release && contacts_[i].have_mapped?contacts_[i].last_mapped:contacts_[i].point;
        return event;
    }
public:
    uint32_t Count() const noexcept { return count_; }
    bool Tracked(uint32_t id) const noexcept { return Find(id)<count_; }
    std::optional<sd_motion_event> Release(int64_t now) {
        if(!count_) return {};
        auto result=Batch(1,now,true);contacts_={};count_=0;down_time_=0;return result;
    }
    std::optional<sd_motion_event> Process(UINT message,const SonarTouchSample& sample,int64_t now) {
        auto index=Find(sample.windows_id);
        if(message==WM_POINTERDOWN) {
            if(index<count_ || count_==contacts_.size() || !sample.captured || !sample.contact || sample.canceled) return {};
            uint32_t used=0;for(uint32_t i=0;i<count_;++i) used|=1u<<contacts_[i].point.pointer_id;
            int32_t id=0;while(used&(1u<<id)) ++id;
            if(!count_) down_time_=now;
            index=count_++;contacts_[index].windows_id=sample.windows_id;contacts_[index].point.pointer_id=id;Store(index,sample);
            return Batch(index?5u|(index<<8):0u,now);
        }
        if(index==count_) return {};
        if(message==WM_POINTERUP) {
            Store(index,sample);auto event=Batch(count_==1?1u:6u|(index<<8),now);
            for(uint32_t i=index+1;i<count_;++i) contacts_[i-1]=contacts_[i];contacts_[--count_]={};
            if(!count_) down_time_=0;return event;
        }
        if(message==WM_POINTERUPDATE && sample.contact && !sample.canceled) { Store(index,sample);return Batch(2,now); }
        return {};
    }
};
}
