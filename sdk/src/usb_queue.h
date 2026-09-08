#pragma once
#include "packet.h"
#include "frame_decoder.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <algorithm>

namespace sd::native {
// SDK role alias for the two instances of APK r2.b (original name unknown).
class SAUsbPacketQueue {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<SANetPacket> packets_;
    size_t bytes_=0,limit_;
    bool closed_=false;
public:
    explicit SAUsbPacketQueue(size_t limit=2ull*max_payload):limit_(limit) {}
    bool Put(SANetPacket packet) {
        { std::lock_guard<std::mutex> lock(mutex_);
          if(closed_) return false;
          if(packets_.size()>=1024 || packet.payload.size()>limit_-std::min(limit_,bytes_))
              throw Failure(SD_OUT_OF_MEMORY,"USB packet queue exceeded its memory budget");
          const auto count=packet.payload.size();packets_.push_back(std::move(packet));bytes_+=count; }
        changed_.notify_one();return true;
    }
    bool Take(SANetPacket& packet) {
        std::unique_lock<std::mutex> lock(mutex_);changed_.wait(lock,[this]{return closed_||!packets_.empty();});
        if(closed_) return false;
        packet=std::move(packets_.front());packets_.pop_front();bytes_-=packet.payload.size();return true;
    }
    void Close() noexcept {
        { std::lock_guard<std::mutex> lock(mutex_);closed_=true;packets_.clear();bytes_=0; }
        changed_.notify_all();
    }
};
}
