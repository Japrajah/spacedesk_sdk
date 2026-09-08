#pragma once
#include "tasks.h"
#include "spacedesk_sdk.h"
#include <cstdint>
#include <functional>

namespace sd::native {
struct SAConnectionWatcherConfig { uint32_t period_ms=0,timeout_ms=20000; };
// Log-confirmed APK v2. Java implements the looped-task interface; the C++ port
// uses the existing equivalent task interface, without claiming a UWP layout.
class SATaskLoopedNetConnectionWatcher final : public PSTaskAsyncLooped {
public:
    enum class Decision { None, Warn, Expire };
    using Clock=std::function<uint64_t()>;
    using Notify=std::function<void(Decision,int32_t)>;
private:
    SAConnectionWatcherConfig config_;
    Clock clock_;
    Notify notify_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool stopped_=false;
    sd_connection_watch_state state_{};
public:
    SATaskLoopedNetConnectionWatcher(SAConnectionWatcherConfig,Notify,Clock={});
    virtual ~SATaskLoopedNetConnectionWatcher()=default;
    bool OnEntry() override;
    bool OnExecute() override;
    void OnExit() noexcept override;
    void RequestExit() noexcept override;
    void Activity();
    void StateGet(sd_connection_watch_state&) const;
    static uint64_t WallClock();
    static int32_t Delta(uint64_t now,uint64_t previous) noexcept;
    static Decision Evaluate(int32_t delta,uint32_t timeout) noexcept;
};
}
