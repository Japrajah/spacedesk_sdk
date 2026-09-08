#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace sd::native {
// SDK value adapters for Android MotionEvent/PointerProperties/PointerCoords.
// Axis slots retain Android IDs; extra axes are copied without interpretation.
struct SonarMotionPoint {
    int32_t id=0,tool=0;
    std::array<float,64> axes{};
    bool coordinates_available=true;
    float Pressure() const noexcept { return axes[2]; }
    float Size() const noexcept { return axes[3]; }
};
struct SonarMotionEvent {
    int64_t down_time=0,event_time=0;
    int32_t action=0,meta_state=0,buttons=0,device=0,edge_flags=0,source=0,flags=0;
    float x_precision=1,y_precision=1;
    std::vector<SonarMotionPoint> points;
    int32_t MaskedAction() const noexcept { return action&255; }
    size_t ActionIndex() const noexcept { return (uint32_t(action)&0xff00)>>8; }
};
struct SonarMotionResult { SonarMotionEvent event;bool rebuilt=false; };
class SonarMotionClock {
public:
    virtual ~SonarMotionClock()=default;
    virtual int64_t NowMillis()=0;
    // Scheduling boundary inside the original 100 ms spin. A serialized Windows
    // runtime can advance complete PCM work here; tests can supply exact ticks.
    virtual void WaitStep(int64_t deadline)=0;
};
int64_t SonarWallTimeMillis();
}
