#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace sd::viewer {
uint64_t ConnectionUptime();
// Viewer owner-thread policy for SAActivityDisplay Handler.postDelayed timers.
// These are retry-budget timers, not delays between connection attempts.
class APKConnectionTimeouts {
    std::vector<uint64_t> deadlines_;
public:
    void Clear() noexcept { deadlines_.clear(); }
    void Schedule(uint32_t delay,uint64_t now) { if(delay) deadlines_.push_back(now+delay); }
    uint32_t Until(uint64_t now) const noexcept {
        if(deadlines_.empty()) return UINT32_MAX;
        const auto next=*std::min_element(deadlines_.begin(),deadlines_.end());
        return next<=now?0:uint32_t(std::min<uint64_t>(UINT32_MAX,next-now));
    }
    // A Connected message queued before the timer must run first, even if the
    // owner did not poll until later (Android Handler ordering).
    bool ConsumeBefore(uint64_t now,uint64_t event_time=UINT64_MAX) noexcept {
        bool due=false;
        for(auto it=deadlines_.begin();it!=deadlines_.end();) {
            if(*it<=now && *it<=event_time) { it=deadlines_.erase(it);due=true; }
            else ++it;
        }
        return due;
    }
};
}
