#pragma once
#include "usb_pipe.h"
#include "usb_queue.h"
#include "net_client.h"
#include "audio_client.h"

namespace sd::native {
// Log-confirmed APK r2.l: distinct JPEG(4) and H264(1) decoder queues.
class SATaskLoopedFrameBufferProcessorUsb final : public PSTaskAsyncLooped {
    SAUsbPacketQueue packets_;
    PSTaskQueue jpeg_queue_{4},h264_queue_{1};
    std::array<std::unique_ptr<SATaskQueuedDecoder>,4> jpeg_;
    SATaskQueuedDecoder h264_{0};
    std::vector<uint32_t> pending_;
    FrameBuffer canvas_;
    uint32_t jpeg_counter_=0;
    bool com_=false;
    std::atomic<bool> stopping_{false};
    SATaskLoopedFrameBufferProcessor::Present present_;
    SATaskLoopedFrameBufferProcessor::Acknowledge acknowledge_;
    SATaskLoopedFrameBufferProcessor::ReportError error_;
    std::vector<protocol::Rectangle> dirty_;
    uint32_t dirty_width_=0,dirty_height_=0;
    bool ApplyJpeg();
    bool Apply(const DecodedFragment*);
    void Present(bool changed);
public:
    SATaskLoopedFrameBufferProcessorUsb(SATaskLoopedFrameBufferProcessor::Present,
        SATaskLoopedFrameBufferProcessor::Acknowledge,SATaskLoopedFrameBufferProcessor::ReportError);
    bool Submit(SANetPacket packet) { return packets_.Put(std::move(packet)); }
    bool OnEntry() override;
    bool OnExecute() override;
    void OnExit() noexcept override;
    void RequestExit() noexcept override;
    virtual ~SATaskLoopedFrameBufferProcessorUsb()=default;
};
class SAUsbClient;
class SATaskLoopedUsbReceiver final : public PSTaskAsyncLooped {
    SAUsbClient& owner_;
    std::shared_ptr<SAUsbPipeStream> pipe_;
    protocol::IdentificationPacket identity_;
    uint64_t generation_;
public:
    SATaskLoopedUsbReceiver(SAUsbClient&,std::shared_ptr<SAUsbPipeStream>,protocol::IdentificationPacket,uint64_t);
    bool OnEntry() override;
    bool OnExecute() override;
    void OnExit() noexcept override;
    void RequestExit() noexcept override;
    virtual ~SATaskLoopedUsbReceiver()=default;
};
// SDK owner/dispatcher adapter for APK r2.j; this is not an inferred RTTI class.
class SAUsbClient {
    friend class SATaskLoopedUsbReceiver;
    class TransmitTask;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<SANetEvent> events_;
    size_t queued_bytes_=0;
    std::atomic<bool> stopping_{true},quiescing_{true},out_of_memory_{false};
    uint64_t generation_=0;
    std::shared_ptr<SAUsbPipeStream> pipe_;
    std::unique_ptr<SATaskLoopedUsbReceiver> receiver_;
    std::unique_ptr<SATaskLoopedFrameBufferProcessorUsb> frames_;
    PSThreadTaskLoop receiver_thread_,frame_thread_;
    PSTaskQueue transmitter_{1};
    SANetAudioClient audio_;
    std::shared_ptr<PSRendererD3D11> renderer_;
    bool audio_enabled_=false;
    bool Post(SANetEvent) noexcept;
    void Failed(uint64_t,sd_result,const char*,uint32_t=0) noexcept;
public:
    SAUsbClient()=default;
    ~SAUsbClient() { Stop(); }
    void Start(const sd_usb_stream&,protocol::IdentificationPacket,bool audio,const sd_audio_config&,
        const std::wstring&,uint32_t timeout,std::shared_ptr<PSRendererD3D11>);
    void Stop() noexcept;
    void PrepareDisconnect() noexcept;
    bool WaitForTransmitter(uint32_t timeout) { return transmitter_.WaitForIdle(timeout); }
    void Transmit(std::vector<uint8_t>,bool priority=false);
    template<class Packet> void Transmit(const Packet& packet,bool priority=false) {
        const auto bytes=protocol::bytes(packet);Transmit({bytes.begin(),bytes.end()},priority);
    }
    bool NextEvent(SANetEvent&,uint32_t timeout);
    SANetAudioClient& Audio() noexcept { return audio_; }
};
}
