#include "tasks.h"
#include <chrono>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sd::native {
PSThreadBase::~PSThreadBase() { Wait(); }
bool PSThreadBase::Start() {
    if (thread_.joinable()) return false;
    try { thread_ = std::thread([this] { Run(); }); return true; }
    catch (...) { return false; }
}
void PSThreadBase::Wait() noexcept {
    // Runtime owners never destroy/join their own worker; no detached lifetimes.
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) std::terminate();
        thread_.join();
    }
}
PSThreadTaskLoop::~PSThreadTaskLoop() { Stop(); }
bool PSThreadTaskLoop::Start(PSTaskAsyncLooped& task) {
    if (Joinable()) return false;
    task_ = &task;
    if (PSThreadBase::Start()) return true;
    task_ = nullptr; return false;
}
void PSThreadTaskLoop::RequestExit() noexcept { if (auto* task = task_.load()) task->RequestExit(); }
void PSThreadTaskLoop::Stop() noexcept { RequestExit(); Wait(); task_ = nullptr; }
void PSThreadTaskLoop::Run() noexcept {
    // Original 0x14001F180: entry once, repeat until false, exit once.
    auto* task = task_.load();
    bool running = false;
    try { running = task->OnEntry(); } catch (...) {}
    while (running) {
        try { running = task->OnExecute(); }
        catch (...) { /* Native runner logs an exception and continues. */ }
    }
    task_.store(nullptr);
    task->OnExit();
}
PSTaskQueue::PSTaskQueue(size_t threads) {
    if (!threads) throw std::invalid_argument("Task queue requires at least one worker");
    for (size_t i = 0; i < threads; ++i) workers_.emplace_back(new PSThreadTaskQueue(*this));
}
PSTaskQueue::~PSTaskQueue() { Stop(false); }
bool PSTaskQueue::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (accepting_) return true;
        stopping_ = false; accepting_ = true; failure_ = nullptr;
    }
    for (auto& worker : workers_) {
        if (!worker->Start()) { Stop(false); return false; }
    }
    return true;
}
bool PSTaskQueue::TaskSubmit(PSTaskAsyncBase* task, bool replace_pending) {
    if (!task) return false;
    std::deque<PSTaskAsyncBase*> removed;
    std::deque<PSTaskAsyncBase*> replacement;
    bool accepted = false;
    try {
        if (replace_pending) replacement.push_back(task);
        std::lock_guard<std::mutex> lock(mutex_);
        if (accepting_) {
            if (replace_pending) {
                removed.swap(pending_);
                pending_.swap(replacement);
            } else pending_.push_back(task);
            accepted = true;
        }
    } catch (...) {
        for (auto* entry : removed) entry->Delete();
        task->Delete();
        return false;
    }
    for (auto* entry : removed) entry->Delete();
    if (!accepted) task->Delete();
    else ready_.notify_all();
    return accepted;
}
PSTaskAsyncBase* PSTaskQueue::Take() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
    if (stopping_) return nullptr;
    auto* task = pending_.front(); pending_.pop_front(); ++active_;
    return task;
}
void PSTaskQueue::Complete(std::exception_ptr failure) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure && !failure_) failure_ = failure;
    --active_;
    if (!active_ && pending_.empty()) idle_.notify_all();
}
void PSThreadTaskQueue::Run() noexcept {
    // Native 0x14001F2F0 / 0x14001E730: FIFO, then cleanup; priority replaces backlog.
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    while (auto* task = queue_.Take()) {
        std::exception_ptr failure;
        try { task->OnExecute(); } catch (...) { failure = std::current_exception(); }
        task->Delete();
        queue_.Complete(failure);
    }
}
bool PSTaskQueue::WaitForIdle(unsigned timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this] { return !active_ && pending_.empty(); });
}
void PSTaskQueue::Stop(bool drain) noexcept {
    std::deque<PSTaskAsyncBase*> removed;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        accepting_ = false;
        if (drain) idle_.wait(lock, [this] { return !active_ && pending_.empty(); });
        stopping_ = true; removed.swap(pending_);
    }
    for (auto* task : removed) task->Delete();
    ready_.notify_all();
    for (auto& worker : workers_) worker->Wait();
    idle_.notify_all();
}
std::exception_ptr PSTaskQueue::Failure() {
    std::lock_guard<std::mutex> lock(mutex_); return failure_;
}
}
