#pragma once
#include "packet.h"
#include "frame_decoder.h"
#include "h264_decoder.h"
#include "tasks.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

namespace sd::native {
struct SAScreenUpdate {
    enum class Kind { Packet, Clear, Ping } kind = Kind::Packet;
    SANetPacket packet;
};
// RTTI .?AVSAQueueScreenUpdates@@. Native uses pointer markers 1/2 for clear/ping;
// the SDK represents those markers with an enum rather than invalid pointers.
class SAQueueScreenUpdates {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<SAScreenUpdate> queue_;
    size_t bytes_ = 0;
    bool closed_ = false;
public:
    bool Submit(SAScreenUpdate update);
    bool Take(SAScreenUpdate& update);
    void Close() noexcept;
};
class SATaskQueuedDecoder final : public PSTaskAsyncBase {
    uint32_t fragment_index_;
    std::optional<SANetPacket> packet_;
    std::optional<DecodedFragment> result_;
    std::exception_ptr failure_;
    PSDecoderMftH264 h264_;
    SADecLibJpeg jpeg_;
public:
    explicit SATaskQueuedDecoder(uint32_t index) : fragment_index_(index) {}
    void Assign(SANetPacket packet);
    bool OnExecute() override;
    // Original processor owns four permanent decoder slots. Base Delete is a
    // no-op; tasks are reused only after the decode queue becomes idle.
    const DecodedFragment* Result() const;
    void Release() noexcept;
    void Shutdown() noexcept;
    virtual ~SATaskQueuedDecoder() = default;
};
class SATaskLoopedFrameBufferProcessor final : public PSTaskAsyncLooped {
public:
    using Present = std::function<bool(std::shared_ptr<const FrameSnapshot>, const std::vector<protocol::Rectangle>&)>;
    using Acknowledge = std::function<void(uint32_t)>;
    using ReportError = std::function<void(sd_result, const char*, uint32_t)>;
private:
    SAQueueScreenUpdates updates_;
    PSTaskQueue decoder_queue_{4};
    std::array<std::unique_ptr<SATaskQueuedDecoder>, 4> decoders_;
    FrameBuffer renderer_;
    std::vector<protocol::Rectangle> dirty_;
    uint32_t dirty_width_ = 0, dirty_height_ = 0;
    std::atomic<bool> exit_requested_{false};
    bool com_initialized_ = false;
    uint32_t fragment_counter_ = 0, pending_acks_ = 0, submitted_ = 0;
    uint32_t expected_count_ = 0, group_width_ = 0, group_height_ = 0;
    Present present_;
    Acknowledge acknowledge_;
    ReportError report_error_;
    bool WaitAndApply();
    void PrivateOnScreenUpdateAvailable(SANetPacket packet);
    void Fail(sd_result status, const char* diagnostic, uint32_t reason = 0) noexcept;
public:
    SATaskLoopedFrameBufferProcessor(Present, Acknowledge, ReportError);
    bool Submit(SANetPacket packet);
    bool Ping();
    bool OnEntry() override;
    bool OnExecute() override;
    void OnExit() noexcept override;
    void RequestExit() noexcept override;
    virtual ~SATaskLoopedFrameBufferProcessor() = default;
};
}
