#pragma once
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sd::native {
// RTTI: .?AVPSTaskAsyncBase@@ / .?AVPSTaskAsyncLooped@@.
// VMT order is significant: Execute, cleanup, then Entry/Exit/RequestExit.
// Delete is a descriptive name for slot 1, whose original spelling is unknown.
class PSTaskAsyncBase {
public:
    virtual bool OnExecute() = 0;
    virtual void Delete() noexcept {}
protected:
    ~PSTaskAsyncBase() = default; // The original base has no virtual destructor.
};
class PSTaskAsyncLooped : public PSTaskAsyncBase {
public:
    virtual bool OnEntry() = 0;
    virtual void OnExit() noexcept = 0;
    virtual void RequestExit() noexcept = 0;
};
class PSThreadBase {
    std::thread thread_;
public:
    PSThreadBase() = default;
    PSThreadBase(const PSThreadBase&) = delete;
    PSThreadBase& operator=(const PSThreadBase&) = delete;
    virtual ~PSThreadBase();
    virtual bool Start();
    void Wait() noexcept;
    bool Joinable() const noexcept { return thread_.joinable(); }
protected:
    virtual void Run() noexcept = 0;
};
class PSThreadTaskLoop final : public PSThreadBase {
    std::atomic<PSTaskAsyncLooped*> task_{nullptr}; // Borrowed, as in the native loop.
    void Run() noexcept override;
public:
    ~PSThreadTaskLoop() override;
    bool Start(PSTaskAsyncLooped& task);
    void RequestExit() noexcept;
    void Stop() noexcept;
};
class PSTaskQueue;
class PSThreadTaskQueue final : public PSThreadBase {
    PSTaskQueue& queue_;
    void Run() noexcept override;
public:
    explicit PSThreadTaskQueue(PSTaskQueue& queue) : queue_(queue) {}
};
class PSTaskQueue {
    friend class PSThreadTaskQueue;
    std::mutex mutex_;
    std::condition_variable ready_, idle_;
    std::deque<PSTaskAsyncBase*> pending_;
    std::vector<std::unique_ptr<PSThreadTaskQueue>> workers_;
    size_t active_ = 0;
    bool accepting_ = false, stopping_ = true;
    std::exception_ptr failure_;
    PSTaskAsyncBase* Take() noexcept;
    void Complete(std::exception_ptr failure) noexcept;
public:
    explicit PSTaskQueue(size_t threads = 1);
    ~PSTaskQueue();
    PSTaskQueue(const PSTaskQueue&) = delete;
    PSTaskQueue& operator=(const PSTaskQueue&) = delete;
    bool Start();
    // Always consumes the task via its virtual cleanup slot, including rejection.
    // replace_pending discards queued tasks, never interrupts the running task.
    bool TaskSubmit(PSTaskAsyncBase* task, bool replace_pending = false);
    bool WaitForIdle(unsigned timeout_ms);
    void Stop(bool drain = true) noexcept;
    std::exception_ptr Failure();
};
}
