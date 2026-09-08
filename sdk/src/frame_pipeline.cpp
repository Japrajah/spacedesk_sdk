#include "frame_pipeline.h"
#include <utility>

namespace sd::native {
bool SAQueueScreenUpdates::Submit(SAScreenUpdate update) {
    const auto size = update.packet.payload.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        if (queue_.size() >= 1024 || bytes_ + size > 2ull * max_payload)
            throw Failure(SD_OUT_OF_MEMORY, "Screen update queue exceeded its memory budget");
        queue_.push_back(std::move(update)); bytes_ += size;
    }
    ready_.notify_one(); return true;
}
bool SAQueueScreenUpdates::Take(SAScreenUpdate& update) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (closed_) return false;
    bytes_ -= queue_.front().packet.payload.size();
    update = std::move(queue_.front()); queue_.pop_front(); return true;
}
void SAQueueScreenUpdates::Close() noexcept {
    { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; queue_.clear(); bytes_ = 0; }
    ready_.notify_all();
}
void SATaskQueuedDecoder::Assign(SANetPacket packet) {
    packet_ = std::move(packet); result_.reset(); failure_ = nullptr;
}
bool SATaskQueuedDecoder::OnExecute() {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        if (FAILED(com)) throw Failure(SD_DECODE_ERROR, "Cannot initialize decoder COM apartment");
        if (!packet_) throw Failure(SD_INTERNAL_ERROR, "Decoder slot has no packet");
        const auto header = protocol::decode<protocol::FramebufferHeader>(packet_->header);
        if (header.compression == protocol::Compression::H264) result_ = h264_.Decode(header, packet_->payload);
        else result_ = DecodeFragment(header, packet_->payload, jpeg_);
    } catch (...) { failure_ = std::current_exception(); }
    if (SUCCEEDED(com)) CoUninitialize();
    return !failure_;
}
const DecodedFragment* SATaskQueuedDecoder::Result() const {
    if (failure_) std::rethrow_exception(failure_);
    return result_ ? &*result_ : nullptr; // H264 NEED_MORE_INPUT is a normal result.
}
void SATaskQueuedDecoder::Release() noexcept { packet_.reset(); result_.reset(); failure_ = nullptr; }
void SATaskQueuedDecoder::Shutdown() noexcept { Release(); h264_.Reset(); }
SATaskLoopedFrameBufferProcessor::SATaskLoopedFrameBufferProcessor(Present present, Acknowledge ack, ReportError error)
    : present_(std::move(present)), acknowledge_(std::move(ack)), report_error_(std::move(error)) {
    for (uint32_t i = 0; i < 4; ++i) decoders_[i] = std::make_unique<SATaskQueuedDecoder>(i);
}
bool SATaskLoopedFrameBufferProcessor::Submit(SANetPacket packet) {
    return updates_.Submit({SAScreenUpdate::Kind::Packet, std::move(packet)});
}
bool SATaskLoopedFrameBufferProcessor::Ping() { return updates_.Submit({SAScreenUpdate::Kind::Ping, {}}); }
bool SATaskLoopedFrameBufferProcessor::OnEntry() {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { Fail(SD_INTERNAL_ERROR, "Cannot initialize processor COM apartment"); return false; }
    com_initialized_ = true;
    if (decoder_queue_.Start()) return true;
    Fail(SD_INTERNAL_ERROR, "Cannot start decode task queue"); return false;
}
bool SATaskLoopedFrameBufferProcessor::WaitAndApply() {
    // Native queue idle barrier at processor +592. Polling the barrier here only
    // permits cancellation to skip presentation; workers are still joined.
    while (!decoder_queue_.WaitForIdle(100)) {}
    if (exit_requested_) return false;
    bool changed = false;
    // Validate every result first: a corrupt tile must not publish a half frame.
    for (uint32_t i = 0; i < submitted_; ++i) decoders_[i]->Result();
    for (uint32_t i = 0; i < submitted_; ++i) {
        if (const auto* result = decoders_[i]->Result()) {
            renderer_.Apply(*result);
            if (dirty_width_ != result->width || dirty_height_ != result->height) {
                // A ping may retain unpresented rectangles across a canvas resize.
                // Upload the new canvas, including cleared areas, instead of old bounds.
                dirty_.clear(); dirty_.push_back({0,0,int32_t(result->width),int32_t(result->height)});
                dirty_width_ = result->width; dirty_height_ = result->height;
            } else if (dirty_.size() >= 64) {
                dirty_.clear(); dirty_.push_back({0,0,int32_t(result->width),int32_t(result->height)});
            } else dirty_.push_back(result->update);
            changed = true;
        }
        decoders_[i]->Release();
    }
    submitted_ = 0;
    return changed;
}
void SATaskLoopedFrameBufferProcessor::PrivateOnScreenUpdateAvailable(SANetPacket packet) {
    const auto h = protocol::decode<protocol::FramebufferHeader>(packet.header);
    const auto count = FragmentCount(h);
    const auto index = fragment_counter_ % count;
    if (index != submitted_ || (submitted_ && (count != expected_count_ || h.width != group_width_ || h.height != group_height_)))
        throw Failure(SD_PROTOCOL_ERROR, "Fragment sequence changed without a ping reset");
    if (!index) {
        if (pending_acks_ >= 256) throw Failure(SD_PROTOCOL_ERROR, "Too many incomplete frame groups");
        expected_count_ = count; group_width_ = h.width; group_height_ = h.height; ++pending_acks_;
    }
    decoders_[index]->Assign(std::move(packet));
    if (!decoder_queue_.TaskSubmit(decoders_[index].get())) throw Failure(SD_INTERNAL_ERROR, "Decode queue stopped");
    ++fragment_counter_; ++submitted_;
    if (index + 1 != count) return;
    const bool changed = WaitAndApply();
    if (exit_requested_) return;
    if (changed) {
        if (!present_(renderer_.Present(), dirty_)) throw Failure(SD_OUT_OF_MEMORY, "Cannot publish decoded frame");
        dirty_.clear();
    }
    acknowledge_(pending_acks_); pending_acks_ = 0;
}
void SATaskLoopedFrameBufferProcessor::Fail(sd_result status, const char* diagnostic, uint32_t reason) noexcept {
    try { report_error_(status, diagnostic, reason); } catch (...) {}
    RequestExit();
}
bool SATaskLoopedFrameBufferProcessor::OnExecute() {
    try {
        SAScreenUpdate update;
        if (!updates_.Take(update)) return false;
        switch (update.kind) {
        case SAScreenUpdate::Kind::Packet: PrivateOnScreenUpdateAvailable(std::move(update.packet)); break;
        case SAScreenUpdate::Kind::Ping:
            // Preserve outstanding ACK count. The barrier additionally protects
            // a reused slot when ping interrupts an in-flight decode group.
            WaitAndApply(); fragment_counter_ = expected_count_ = 0; break;
        case SAScreenUpdate::Kind::Clear: WaitAndApply(); renderer_.Clear(); dirty_.clear(); dirty_width_ = dirty_height_ = 0; break;
        }
        return !exit_requested_;
    } catch (const Failure& error) { Fail(error.status, error.what(), error.disconnect_reason); }
      catch (const std::bad_alloc&) { Fail(SD_OUT_OF_MEMORY, "Frame pipeline allocation failed"); }
      catch (...) { Fail(SD_INTERNAL_ERROR, "Frame pipeline task failed"); }
    return false;
}
void SATaskLoopedFrameBufferProcessor::OnExit() noexcept {
    decoder_queue_.Stop(true);
    for (auto& decoder : decoders_) decoder->Shutdown();
    if (com_initialized_) CoUninitialize();
    com_initialized_ = false;
}
void SATaskLoopedFrameBufferProcessor::RequestExit() noexcept {
    exit_requested_ = true; updates_.Close();
}
}
