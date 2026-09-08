#pragma once
#include "audio_renderer.h"
#include "net_client.h"
#include "usb_queue.h"
#include <deque>

namespace sd::native {
class SANetAudioClient;
// APK r2.k, the separate renderer task consuming USB-multiplexed PCM packets.
class SATaskLoopedUsbAudioRenderer final : public PSTaskAsyncLooped {
    SANetAudioClient& owner_;
    SAUsbPacketQueue packets_{4*1024*1024};
    Microsoft::WRL::ComPtr<SAAudioRenderer> renderer_;
    sd_audio_config config_{};
    std::wstring endpoint_;
    bool com_=false;
public:
    SATaskLoopedUsbAudioRenderer(SANetAudioClient&,const sd_audio_config&,std::wstring);
    bool Submit(SANetPacket packet) { return packets_.Put(std::move(packet)); }
    bool OnEntry() override;
    bool OnExecute() override;
    void OnExit() noexcept override;
    void RequestExit() noexcept override;
    virtual ~SATaskLoopedUsbAudioRenderer()=default;
};
class SATaskLoopedNetAudioReceiverRenderer final : public PSTaskAsyncLooped {
    SANetAudioClient& owner_;
    std::shared_ptr<SANetSocketStream> socket_;
    Microsoft::WRL::ComPtr<SAAudioRenderer> renderer_;
    sd_audio_config config_{};
    std::wstring endpoint_;
    bool com_ = false;
public:
    SATaskLoopedNetAudioReceiverRenderer(SANetAudioClient&,std::shared_ptr<SANetSocketStream>,const sd_audio_config&,std::wstring);
    bool OnEntry() override;
    bool OnExecute() override;
    void OnExit() noexcept override;
    void RequestExit() noexcept override;
    void CancelRenderer() noexcept { renderer_->Cancel(); }
    virtual ~SATaskLoopedNetAudioReceiverRenderer() = default;
};
class SATaskQueuedNetAudioConnect final : public PSTaskAsyncBase {
    SANetAudioClient& owner_;
    std::shared_ptr<SANetSocketStream> socket_;
    std::string address_;
    uint16_t port_;
    protocol::AudioIdentificationPacket identity_;
    bool probe_;
    uint32_t probe_timeout_;
public:
    SATaskQueuedNetAudioConnect(SANetAudioClient&,std::shared_ptr<SANetSocketStream>,std::string,uint16_t,
                               const protocol::IdentificationData&,bool,uint32_t);
    bool OnExecute() override;
    void Delete() noexcept override { delete this; }
    virtual ~SATaskQueuedNetAudioConnect() = default;
};
// SDK owner adapter for native SANetClient's separate audio queue/receiver.
class SANetAudioClient {
    friend class SATaskLoopedNetAudioReceiverRenderer;
    friend class SATaskQueuedNetAudioConnect;
    friend class SATaskLoopedUsbAudioRenderer;
    struct Block { std::vector<int16_t> samples;uint64_t sequence=0,frame_index=0; };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    sd_audio_state state_{};
    sd_audio_disconnect_state disconnect_state_{sizeof(sd_audio_disconnect_state),0,0,0};
    std::string error_;
    std::deque<std::shared_ptr<const Block>> queue_;
    std::shared_ptr<const Block> delivered_;
    uint64_t queue_frames_=0,capacity_frames_=0;
    std::atomic<bool> stopping_{true},connected_{false};
    uint32_t io_timeout_=1000;
    PSTaskQueue transmitter_{1};
    sd_tls_state last_tls_state_{};
    PSThreadTaskLoop receiver_thread_;
    std::unique_ptr<SATaskLoopedNetAudioReceiverRenderer> receiver_;
    std::unique_ptr<SATaskLoopedUsbAudioRenderer> usb_receiver_;
    std::shared_ptr<SANetSocketStream> socket_;
    void StartReceiver();
    void Ready(uint32_t channels,bool playback);
    void Failed(sd_result,const char*,HRESULT=0) noexcept;
    void Closed() noexcept;
    void ServerDisconnected(uint32_t) noexcept;
    uint64_t PacketReceived();
    void Push(const std::vector<uint8_t>&,uint64_t);
public:
    SANetAudioClient() = default;
    ~SANetAudioClient() { Stop(); }
    sd_result Start(const std::string&,uint16_t,const protocol::IdentificationData&,const sd_audio_config&,
                    const std::wstring&,uint32_t connect_timeout,uint32_t io_timeout,const SASecureSocketLayerConfig& tls = {}) noexcept;
    void TlsStateGet(sd_tls_state&) const;
    sd_result StartUsb(const sd_audio_config&,const std::wstring&) noexcept;
    bool SubmitUsb(SANetPacket packet) noexcept;
    void Stop() noexcept;
    bool Stopping() const noexcept { return stopping_; }
    void StateGet(sd_audio_state&) const;
    void DisconnectStateGet(sd_audio_disconnect_state&) const;
    void ClearDisconnectState() noexcept;
    std::string LastError() const;
    sd_result Poll(uint32_t,sd_audio_buffer&);
};
}
