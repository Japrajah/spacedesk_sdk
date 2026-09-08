#include "spacedesk_sdk.h"
#include <cmath>
#include <cstring>

namespace {
bool valid(const sd_orientation_state* state) {
    const auto orientation=[](int32_t value) { return value==-1 || (value>=1 && value<=4); };
    return state && state->struct_size==sizeof(*state) && state->natural_portrait<=1 &&
        state->last_angle>=0 && state->last_angle<360 && orientation(state->candidate_orientation) && orientation(state->applied_orientation);
}
// Role port of SAActivityDisplay/Usb.onSensorChanged. Intermediate float32
// operations and the source constant are retained before Java Math.round.
void sensor(sd_orientation_state& state,float x,float y,float z) {
    const float xx=(-x)*(-x),yy=(-y)*(-y),sum=xx+yy,planar=sum*4.0f,zz=(-z)*(-z);
    if(!(planar>=zz)) return; // cmpl-float also rejects unordered/NaN.
    float radians=float(std::atan2(double(y),double(-x)));
    const uint32_t bits=0x42652ee1;float degrees;std::memcpy(&degrees,&bits,sizeof(degrees));
    const float scaled=radians*degrees;
    int32_t angle=90-int32_t(std::floor(double(scaled)+.5));
    while(angle>=360) angle-=360;while(angle<0) angle+=360;
    if(angle==state.last_angle) return;
    state.last_angle=angle;
    int32_t candidate=0;
    if(angle<35 || angle>325) candidate=state.natural_portrait?2:1;
    else if(angle>55 && angle<=125) candidate=state.natural_portrait?3:2;
    else if(angle>145 && angle<=215) candidate=state.natural_portrait?4:3;
    else if(angle>235 && angle<=305) candidate=state.natural_portrait?1:4;
    if(candidate) state.candidate_orientation=candidate;
}
}
extern "C" {
sd_result SD_CALL sd_orientation_init(sd_orientation_state* state,int32_t configuration,int32_t rotation) {
    if(!state) return SD_INVALID_ARGUMENT;
    uint32_t portrait=1;
    if(rotation==0 || rotation==2) portrait=configuration==1;
    else if(rotation==1 || rotation==3) portrait=configuration==2;
    *state={sizeof(*state),portrait,0,-1,-1};return SD_OK;
}
sd_result SD_CALL sd_orientation_sensor_update(sd_orientation_state* state,float x,float y,float z,uint32_t enabled) {
    if(!valid(state) || enabled>1) return SD_INVALID_ARGUMENT;
    if(enabled) sensor(*state,x,y,z);return SD_OK;
}
sd_result SD_CALL sd_orientation_configuration_update(sd_orientation_state* state,int32_t configuration,uint32_t enabled,uint32_t* orientation) {
    if(!valid(state) || enabled>1 || !orientation) return SD_INVALID_ARGUMENT;
    if(!enabled) return SD_IGNORED;
    int32_t selected=-1;
    if(configuration==1) selected=state->candidate_orientation==4?4:2;
    else if(configuration==2) selected=state->candidate_orientation==3?3:1;
    if(selected==-1) return SD_IGNORED;
    state->applied_orientation=selected;*orientation=uint32_t(selected);return SD_OK;
}
}
