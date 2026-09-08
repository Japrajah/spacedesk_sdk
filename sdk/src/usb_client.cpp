#include "usb_client.h"
#include "renderer_d3d11.h"
#include <algorithm>
#include <chrono>

namespace sd::native {
SATaskLoopedFrameBufferProcessorUsb::SATaskLoopedFrameBufferProcessorUsb(SATaskLoopedFrameBufferProcessor::Present present,
    SATaskLoopedFrameBufferProcessor::Acknowledge ack,SATaskLoopedFrameBufferProcessor::ReportError error)
    :present_(std::move(present)),acknowledge_(std::move(ack)),error_(std::move(error)) {
    for(uint32_t i=0;i<4;++i) jpeg_[i]=std::make_unique<SATaskQueuedDecoder>(i);
}
bool SATaskLoopedFrameBufferProcessorUsb::OnEntry() {
    const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);com_=SUCCEEDED(hr);
    if(FAILED(hr) || !jpeg_queue_.Start() || !h264_queue_.Start()) {
        error_(SD_INTERNAL_ERROR,"Cannot initialize USB decoder tasks",0);return false;
    }
    return !stopping_;
}
bool SATaskLoopedFrameBufferProcessorUsb::Apply(const DecodedFragment* frame) {
    if(!frame) return false;
    canvas_.Apply(*frame);
    if(dirty_width_!=frame->width || dirty_height_!=frame->height || dirty_.size()>=64) {
        dirty_width_=frame->width;dirty_height_=frame->height;dirty_.clear();
        dirty_.push_back({0,0,int32_t(frame->width),int32_t(frame->height)});
    } else dirty_.push_back(frame->update);
    return true;
}
bool SATaskLoopedFrameBufferProcessorUsb::ApplyJpeg() {
    while(!jpeg_queue_.WaitForIdle(100)) {}
    if(stopping_) return false;
    for(const auto index:pending_) jpeg_[index]->Result();
    bool changed=false;
    for(const auto index:pending_) { changed=Apply(jpeg_[index]->Result())||changed;jpeg_[index]->Release(); }
    pending_.clear();return changed;
}
void SATaskLoopedFrameBufferProcessorUsb::Present(bool changed) {
    if(stopping_) return;
    if(changed && !present_(canvas_.Present(),dirty_)) throw Failure(SD_OUT_OF_MEMORY,"Cannot publish USB framebuffer");
    dirty_.clear();acknowledge_(1);
}
bool SATaskLoopedFrameBufferProcessorUsb::OnExecute() {
    try {
        SANetPacket packet;if(!packets_.Take(packet) || stopping_) return false;
        auto header=protocol::decode<protocol::FramebufferHeader>(packet.header);
        // APK USB grouping consults legacy +64 only; +60 is not its count field.
        header.fragment_count=0;packet.header=protocol::bytes(header);
        if(header.compression==protocol::Compression::H264) {
            // Serialize pending JPEG canvas writes before a codec switch; keep
            // the native USB counter and issue only this presentation's ACK.
            const bool prior=ApplyJpeg();h264_.Assign(std::move(packet));
            if(!h264_queue_.TaskSubmit(&h264_)) throw Failure(SD_INTERNAL_ERROR,"USB H264 queue stopped");
            while(!h264_queue_.WaitForIdle(100)) {}
            if(stopping_) return false;
            const bool changed=Apply(h264_.Result())||prior;h264_.Release();Present(changed);
        } else if(header.compression==protocol::Compression::MJPEG) {
            const uint32_t index=jpeg_counter_%4;
            if(std::find(pending_.begin(),pending_.end(),index)!=pending_.end())
                throw Failure(SD_PROTOCOL_ERROR,"USB JPEG slot reused before the presentation barrier");
            jpeg_[index]->Assign(std::move(packet));pending_.push_back(index);
            if(!jpeg_queue_.TaskSubmit(jpeg_[index].get())) throw Failure(SD_INTERNAL_ERROR,"USB JPEG queue stopped");
            if(header.four_fragments) ++jpeg_counter_; // Singles do not advance the APK counter.
            if(!header.four_fragments || index==3) Present(ApplyJpeg());
        } else throw Failure(SD_UNSUPPORTED,"APK USB framebuffer supports MJPEG/H264 only");
        return !stopping_;
    } catch(const Failure& e) { error_(e.status,e.what(),e.disconnect_reason); }
      catch(const std::bad_alloc&) { error_(SD_OUT_OF_MEMORY,"USB framebuffer allocation failed",0); }
      catch(...) { error_(SD_INTERNAL_ERROR,"USB framebuffer processing failed",0); }
    RequestExit();return false;
}
void SATaskLoopedFrameBufferProcessorUsb::RequestExit() noexcept { stopping_=true;packets_.Close(); }
void SATaskLoopedFrameBufferProcessorUsb::OnExit() noexcept {
    jpeg_queue_.Stop(true);h264_queue_.Stop(true);
    for(auto& decoder:jpeg_) decoder->Shutdown();h264_.Shutdown();
    if(com_) CoUninitialize();com_=false;
}
SATaskLoopedUsbReceiver::SATaskLoopedUsbReceiver(SAUsbClient& owner,std::shared_ptr<SAUsbPipeStream> pipe,
    protocol::IdentificationPacket identity,uint64_t generation):owner_(owner),pipe_(std::move(pipe)),identity_(identity),generation_(generation) {}
bool SATaskLoopedUsbReceiver::OnEntry() {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
    try {
        const auto initial=protocol::bytes(protocol::UsbInitialIdentification(identity_.header));
        pipe_->Send({initial.begin(),initial.end()});
        const auto full=protocol::bytes(identity_);pipe_->Send({full.begin(),full.end()});
        return owner_.Post({SANetEvent::Kind::Connected,generation_});
    } catch(const Failure& e) { owner_.Failed(generation_,e.status,e.what()); }
      catch(...) { owner_.Failed(generation_,SD_INTERNAL_ERROR,"USB identification task failed"); }
    return false;
}
bool SATaskLoopedUsbReceiver::OnExecute() {
    try {
        SANetEvent event{SANetEvent::Kind::Packet,generation_};if(!pipe_->Receive(event.packet)) return false;
        if(owner_.quiescing_) return true;
        const auto header=protocol::decode<protocol::PacketHeader>(event.packet.header);
        switch(header.prefix.type) {
        case protocol::HeaderType::FRAMEBUFFER:return owner_.frames_->Submit(std::move(event.packet));
        case protocol::HeaderType::AUDIO:
            if(owner_.audio_enabled_) owner_.audio_.SubmitUsb(std::move(event.packet));
            return true;
        case protocol::HeaderType::PING: {
            // USB replies to every ping, unlike the TCP option-specific branch.
            const auto reply=protocol::bytes(protocol::PingHeader{protocol::PingOption::ECHO_REPLY});
            pipe_->Send({reply.begin(),reply.end()});return owner_.Post(std::move(event));
        }
        case protocol::HeaderType::VISIBILITY:case protocol::HeaderType::DISPLAY_SETTINGS:case protocol::HeaderType::DISCONNECT:
            return owner_.Post(std::move(event));
        default:return true; // Native USB receiver ignores other zero-payload controls.
        }
    } catch(const Failure& e) { owner_.Failed(generation_,e.status,e.what()); }
      catch(const std::bad_alloc&) { owner_.Failed(generation_,SD_OUT_OF_MEMORY,"USB receive allocation failed"); }
      catch(...) { owner_.Failed(generation_,SD_INTERNAL_ERROR,"USB receive callback failed"); }
    return false;
}
void SATaskLoopedUsbReceiver::OnExit() noexcept { pipe_->Cancel();owner_.Post({SANetEvent::Kind::Disconnected,generation_}); }
void SATaskLoopedUsbReceiver::RequestExit() noexcept { pipe_->Cancel(); }

class SAUsbClient::TransmitTask final : public PSTaskAsyncBase {
    SAUsbClient& owner_;std::shared_ptr<SAUsbPipeStream> pipe_;std::vector<uint8_t> data_;uint64_t generation_;
public:
    TransmitTask(SAUsbClient& owner,std::shared_ptr<SAUsbPipeStream> pipe,std::vector<uint8_t> data,uint64_t generation)
        :owner_(owner),pipe_(std::move(pipe)),data_(std::move(data)),generation_(generation) {}
    bool OnExecute() override {
        try { pipe_->Send(data_);return true; }
        catch(const Failure& e) { owner_.Failed(generation_,e.status,e.what()); }
        catch(...) { owner_.Failed(generation_,SD_INTERNAL_ERROR,"USB transmit callback failed"); }
        return false;
    }
    void Delete() noexcept override { delete this; }
};
void SAUsbClient::Start(const sd_usb_stream& callbacks,protocol::IdentificationPacket identity,bool audio,
    const sd_audio_config& config,const std::wstring& endpoint,uint32_t timeout,std::shared_ptr<PSRendererD3D11> renderer) {
    Stop();
    // Allocation failure here leaves callbacks unaccepted. After this point
    // failures are asynchronous and release happens through owner-thread Stop.
    pipe_=std::make_shared<SAUsbPipeStream>(callbacks,timeout);
    stopping_=false;quiescing_=false;audio_enabled_=audio;renderer_=std::move(renderer);
    identity.header.session_type=audio?6:4;
    const auto generation=generation_;
    try {
        if(audio) audio_.StartUsb(config,endpoint); // An output failure does not kill the display stream.
        frames_=std::make_unique<SATaskLoopedFrameBufferProcessorUsb>(
            [this,generation](std::shared_ptr<const FrameSnapshot> frame,const std::vector<protocol::Rectangle>& dirty) {
                if(stopping_) return true;
                if(renderer_) {
                    try { renderer_->OnFrame(frame,dirty); }
                    catch(const GpuFailure& e) {
                        renderer_->MarkFailed(e.hresult);SANetEvent failed{SANetEvent::Kind::RendererError,generation};
                        failed.status=e.hresult;failed.diagnostic=e.what();Post(std::move(failed));
                    }
                }
                SANetEvent event{SANetEvent::Kind::Frame,generation};event.frame=std::move(frame);return Post(std::move(event));
            },[this](uint32_t count) { if(!quiescing_) while(count--) Transmit(protocol::FlowControlAckHeader{}); },
            [this,generation](sd_result code,const char* message,uint32_t reason) { Failed(generation,code,message,reason); });
        if(!transmitter_.Start() || !frame_thread_.Start(*frames_)) throw Failure(SD_INTERNAL_ERROR,"Cannot start USB processing tasks");
        receiver_=std::make_unique<SATaskLoopedUsbReceiver>(*this,pipe_,identity,generation);
        if(!receiver_thread_.Start(*receiver_)) throw Failure(SD_INTERNAL_ERROR,"Cannot start USB receiver");
    } catch(const Failure& e) { Failed(generation,e.status,e.what());pipe_->Cancel(); }
      catch(...) { Failed(generation,SD_OUT_OF_MEMORY,"Cannot initialize USB session");pipe_->Cancel(); }
}
void SAUsbClient::PrepareDisconnect() noexcept { quiescing_=true;frame_thread_.Stop(); }
void SAUsbClient::Stop() noexcept {
    stopping_=true;quiescing_=true;
    if(pipe_) pipe_->Cancel();
    receiver_thread_.Stop();frame_thread_.Stop();transmitter_.Stop(false);audio_.Stop();
    receiver_.reset();frames_.reset();pipe_.reset();
    if(renderer_) renderer_->ResetStream();renderer_.reset();
    { std::lock_guard<std::mutex> lock(mutex_);events_.clear();queued_bytes_=0;out_of_memory_=false;++generation_; }
}
void SAUsbClient::Transmit(std::vector<uint8_t> bytes,bool priority) {
    if(!pipe_ || stopping_) throw Failure(SD_IO_ERROR,"USB session is not connected");
    if(!transmitter_.TaskSubmit(new TransmitTask(*this,pipe_,std::move(bytes),generation_),priority))
        throw Failure(SD_IO_ERROR,"USB transmit queue stopped");
}
bool SAUsbClient::Post(SANetEvent event) noexcept {
    if(stopping_) return false;
    bool posted=false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto bytes=event.packet.payload.size()+(event.frame?event.frame->pixels.size():0);
        if(event.kind==SANetEvent::Kind::Frame) for(auto i=events_.begin();i!=events_.end();) {
            if(i->kind==SANetEvent::Kind::Frame) { queued_bytes_-=i->frame->pixels.size();i=events_.erase(i); } else ++i;
        }
        if(events_.size()>=1024 || queued_bytes_+bytes>2ull*max_payload) out_of_memory_=true;
        else { events_.push_back(std::move(event));queued_bytes_+=bytes;posted=true; }
    } catch(...) { out_of_memory_=true; }
    changed_.notify_all();return posted;
}
void SAUsbClient::Failed(uint64_t generation,sd_result code,const char* message,uint32_t reason) noexcept {
    if(stopping_) return;
    try { SANetEvent event{SANetEvent::Kind::Error,generation};event.status=code;event.diagnostic=message;event.disconnect_reason=reason;Post(std::move(event)); }
    catch(...) { out_of_memory_=true;changed_.notify_all(); }
}
bool SAUsbClient::NextEvent(SANetEvent& out,uint32_t timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if(!changed_.wait_for(lock,std::chrono::milliseconds(timeout),[this]{return !events_.empty()||out_of_memory_.load();})) return false;
    if(out_of_memory_.exchange(false)) throw Failure(SD_OUT_OF_MEMORY,"USB event queue exceeded its memory budget");
    while(!events_.empty()) {
        queued_bytes_-=events_.front().packet.payload.size()+(events_.front().frame?events_.front().frame->pixels.size():0);
        out=std::move(events_.front());events_.pop_front();if(out.generation==generation_) return true;
    }
    return false;
}
}
