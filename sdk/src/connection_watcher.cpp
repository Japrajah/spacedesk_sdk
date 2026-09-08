#include "connection_watcher.h"
#include <windows.h>
#include <chrono>
#include <cstring>

namespace sd::native {
uint64_t SATaskLoopedNetConnectionWatcher::WallClock() {
    FILETIME time{};GetSystemTimeAsFileTime(&time);
    const uint64_t ticks=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    return (ticks-116444736000000000ULL)/10000;
}
int32_t SATaskLoopedNetConnectionWatcher::Delta(uint64_t now,uint64_t previous) noexcept {
    const auto low=uint32_t(now-previous);int32_t value;
    std::memcpy(&value,&low,sizeof(value));return value; // Java (int)(long difference)
}
SATaskLoopedNetConnectionWatcher::Decision SATaskLoopedNetConnectionWatcher::Evaluate(int32_t delta,uint32_t timeout) noexcept {
    if(int64_t(delta)>timeout) return Decision::Expire;
    if(int64_t(delta)*2>timeout) return Decision::Warn;
    return Decision::None;
}
SATaskLoopedNetConnectionWatcher::SATaskLoopedNetConnectionWatcher(SAConnectionWatcherConfig config,Notify notify,Clock clock)
    :config_(config),clock_(clock?std::move(clock):Clock(WallClock)),notify_(std::move(notify)) {
    state_.struct_size=sizeof(state_);state_.enabled=config.period_ms!=0;
}
bool SATaskLoopedNetConnectionWatcher::OnEntry() {
    std::lock_guard<std::mutex> lock(mutex_);
    // A pre-start RequestExit must not be lost when the worker begins later.
    if(stopped_ || !config_.period_ms) return false;
    state_.running=1;state_.timed_out=0;state_.warnings=0;state_.last_activity_unix_ms=clock_();return true;
}
bool SATaskLoopedNetConnectionWatcher::OnExecute() {
    Decision decision;int32_t delta;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopped_) return false;
        state_.last_check_unix_ms=clock_();
        delta=Delta(state_.last_check_unix_ms,state_.last_activity_unix_ms);
        state_.elapsed_ms=delta;decision=Evaluate(delta,config_.timeout_ms);
        if(decision==Decision::Warn) ++state_.warnings;
        if(decision==Decision::Expire) state_.timed_out=1;
    }
    if(decision!=Decision::None) notify_(decision,delta);
    if(decision==Decision::Expire) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock,std::chrono::milliseconds(config_.period_ms),[this]{return stopped_;});
    return !stopped_;
}
void SATaskLoopedNetConnectionWatcher::Activity() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!stopped_ && state_.running) state_.last_activity_unix_ms=clock_();
}
void SATaskLoopedNetConnectionWatcher::OnExit() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);state_.last_activity_unix_ms=0;state_.running=0;
}
void SATaskLoopedNetConnectionWatcher::RequestExit() noexcept {
    { std::lock_guard<std::mutex> lock(mutex_);stopped_=true;state_.last_activity_unix_ms=0; }
    changed_.notify_all();
}
void SATaskLoopedNetConnectionWatcher::StateGet(sd_connection_watch_state& out) const {
    std::lock_guard<std::mutex> lock(mutex_);out=state_;
}
}
