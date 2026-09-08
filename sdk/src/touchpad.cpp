#include "touchpad.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace sd::native {
SAMotionEvent::SAMotionEvent(uint32_t raw_action, const SAMotionContact* contacts, size_t count)
    : action_(raw_action) {
    Select(contacts,count);
}
void SAMotionEvent::Select(const SAMotionContact* contacts,size_t count) {
    if (!contacts || !count) return;
    primary_id_ = contacts[0].pointer_id;
    contacts_.reserve(count);
    for (size_t index = 0; index < count; ++index)
        if (contacts[index].mapped) contacts_.push_back(contacts[index]);
    if (contacts[0].mapped) {
        x_ = contacts[0].x; y_ = contacts[0].y;
    } else {
        if (contacts_.empty()) return;
        x_ = contacts_.front().x; y_ = contacts_.front().y;
        // Original a() removes the fallback coordinates from its list but e()
        // still reads pointer ID zero from the original Android MotionEvent.
        contacts_.erase(contacts_.begin());
    }
    valid_ = true;
}
namespace {
// Java float/double-to-int saturates, while integer subtraction wraps. Keep
// these defined even for raw off-view MotionEvent coordinates and tiny ratios.
int32_t java_integer(double value) noexcept {
    if(std::isnan(value)) return 0;
    if(value>=INT32_MAX) return INT32_MAX;
    if(value<=INT32_MIN) return INT32_MIN;
    return int32_t(value);
}
int32_t java_subtract(int32_t a,int32_t b) noexcept {
    const uint32_t bits=uint32_t(a)-uint32_t(b);int32_t value;std::memcpy(&value,&bits,sizeof(value));return value;
}
}
bool SAMotionEvent::Map(const sd_motion_geometry& geometry,int32_t& x,int32_t& y) noexcept {
    const bool sideways=geometry.orientation==2 || geometry.orientation==4;
    // Activity.W0 swaps constructor dimensions for portrait orientation, but
    // W1's packet dimensions remain the actual viewport width/height.
    const int32_t width=int32_t(sideways?geometry.viewport_height:geometry.viewport_width);
    const int32_t height=int32_t(sideways?geometry.viewport_width:geometry.viewport_height);
    const float ratio=float(width)/float(height),server=geometry.server_aspect_ratio;
    if(server>0 && server!=ratio) {
        const bool narrower=server<ratio;
        const int32_t half=(narrower?width:height)/2;
        const float center=float(half);
        const float extent=(narrower?server/ratio:ratio/server)*center;
        const float scale=center/extent;
        const bool x_axis=sideways?!narrower:narrower;
        const int32_t position=x_axis?x:y;
        if(float(position)<center-extent || float(position)>extent+center) return false;
        const float delta=float(position>=half?java_subtract(position,half):java_subtract(half,position))*scale;
        const int32_t mapped=java_integer(position>=half?center+delta:center-delta);
        if(x_axis) x=mapped;else y=mapped;
    }
    const int32_t before_x=x,before_y=y;
    switch(geometry.orientation) {
    case 2:
        x=java_integer((double(before_y)/width)*height);
        y=java_subtract(width,java_integer((double(before_x)/height)*width));break;
    case 3:x=java_subtract(width,before_x);y=java_subtract(height,before_y);break;
    case 4:
        x=java_subtract(height,java_integer((double(before_y)/width)*height));
        y=java_integer((double(before_x)/height)*width);break;
    }
    return true;
}
SAMotionEvent::SAMotionEvent(uint32_t raw_action,const sd_motion_geometry& geometry,const SAMotionContact* contacts,size_t count)
    :action_(raw_action) {
    if(!contacts || !count) return;
    std::vector<SAMotionContact> mapped(contacts,contacts+count);
    for(auto& contact:mapped) if(contact.mapped) contact.mapped=Map(geometry,contact.x,contact.y);
    Select(mapped.data(),mapped.size());
}
SAMotionEvent::SAMotionEvent(const sd_motion_geometry& geometry,const sd_motion_event& input)
    :action_(input.action) {
    if(!input.pointer_count) return;
    // Original g() reads MotionEvent.getPressure() at index zero, even when
    // a() falls back to another pointer for its coordinates.
    pressure_=input.pointers[0].axes[SD_MOTION_AXIS_PRESSURE];
    std::vector<SAMotionContact> contacts;
    contacts.reserve(input.pointer_count);
    for(uint32_t i=0;i<input.pointer_count;++i) {
        const auto& point=input.pointers[i];
        SAMotionContact contact{uint32_t(point.pointer_id),java_integer(point.axes[SD_MOTION_AXIS_X]),java_integer(point.axes[SD_MOTION_AXIS_Y]),true};
        contact.mapped=Map(geometry,contact.x,contact.y);contacts.push_back(contact);
    }
    Select(contacts.data(),contacts.size());
}

SAMotionEvent::SAMotionEvent(const sd_motion_event& input,uint32_t mask):action_(input.action) {
    if(!input.pointer_count) return;
    pressure_=input.pointers[0].axes[SD_MOTION_AXIS_PRESSURE];
    std::vector<SAMotionContact> contacts;contacts.reserve(input.pointer_count);
    for(uint32_t i=0;i<input.pointer_count;++i) {
        const auto& point=input.pointers[i];
        contacts.push_back({uint32_t(point.pointer_id),java_integer(point.axes[SD_MOTION_AXIS_X]),java_integer(point.axes[SD_MOTION_AXIS_Y]),(mask&(1u<<i))!=0});
    }
    Select(contacts.data(),contacts.size());
}
std::optional<protocol::TouchHeader> APKTouchOutput::Build(const SAMotionEvent& event, uint64_t wallclock_ms) const {
    if (!event.Valid() || mode_ > 1 || event.Action() > 3) return std::nullopt;
    // W1 compares getAction() directly: POINTER_DOWN/UP and index-encoded actions
    // are ignored. Masking these actions would change the original protocol.
    const uint16_t relative_flags = event.Action() == 0 ? 2 :
        (event.Action() == 1 || event.Action() == 3) ? 4 : 0;
    protocol::TouchHeader packet{};
    packet.surface_width = width_; packet.surface_height = height_;
    packet.x = event.X(); packet.y = event.Y();
    packet.contact_id = static_cast<uint16_t>(event.PrimaryId());
    packet.timestamp_ms = static_cast<uint32_t>(wallclock_ms);
    packet.flags = uint16_t(relative_flags | (mode_ == 0 ? 1 : 0));
    if (mode_ == 0) {
        const auto& contacts = event.Contacts();
        const auto end = std::min<size_t>(contacts.size(), protocol::max_additional_contacts + 1);
        // Index 1 is intentional even when SAMotionEvent removed a fallback
        // contact. This reproduces the APK's observed fallback/list quirk.
        for (size_t index = 1; index < end; ++index) {
            const auto& source = contacts[index];
            packet.additional[packet.additional_count++] =
                {source.x, source.y, packet.flags, static_cast<uint16_t>(source.pointer_id)};
        }
    }
    return packet;
}
}
