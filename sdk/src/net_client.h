#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include "frame_decoder.h"
#include "frame_pipeline.h"
#include "tasks.h"
#include "tls.h"
#include "connection_watcher.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace sd::native {
class PSRendererD3D11;
struct SANetEvent {
    enum class Kind { Connected, Packet, Frame, Disconnected, Error, RendererError, ConnectionWarning } kind;
    uint64_t generation = 0;
    sd_result status = SD_OK;
    uint32_t disconnect_reason = 0;
    std::string diagnostic;
    SANetPacket packet;
    std::shared_ptr<const FrameSnapshot> frame;
};
class SANetSocketStream {
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::atomic<bool> closing_{false};
    std::atomic<ULONGLONG> drain_until_{0};
    uint32_t connect_timeout_, io_timeout_;
    const uint32_t transport_;
    const bool audio_;
    std::unique_ptr<SASecureSocketLayer> tls_;
    std::mutex send_mutex_;
    bool websocket_close_sent_ = false; // protected by send_mutex_
    bool Wait(bool write, ULONGLONG deadline);
    void SslConnect(ULONGLONG deadline);
    bool ReceiveExact(uint8_t* data, size_t count, ULONGLONG& started);
    void SendBytes(const std::vector<uint8_t>& bytes);
    void WebSocketConnect(const std::string& host, uint16_t port);
    void WebSocketControlSend(uint8_t opcode, const std::vector<uint8_t>& payload);
    bool WebSocketMessageReceive(std::vector<uint8_t>& message);
public:
    SANetSocketStream(uint32_t connect_timeout, uint32_t io_timeout, uint32_t transport, bool audio = false,
                      const SASecureSocketLayerConfig& tls = {})
        : connect_timeout_(connect_timeout), io_timeout_(io_timeout), transport_(transport), audio_(audio) {
        if(tls.mode) tls_=std::make_unique<SASecureSocketLayer>(tls);
    }
    ~SANetSocketStream();
    void TcpCreateConnect(const std::string& address, uint16_t port);
    void Send(const std::vector<uint8_t>& bytes);
    bool ReceiveTcpStream(SANetPacket& packet);
    void Close() noexcept;
    void ShutdownSend(uint32_t grace_ms) noexcept;
    void TlsStateGet(sd_tls_state& state) const;
};
class SANetClient;
class SATaskQueuedNetConnect final : public PSTaskAsyncBase {
    SANetClient& client_;
    std::shared_ptr<SANetSocketStream> socket_;
    std::string address_;
    uint16_t port_;
    uint64_t generation_;
public:
    SATaskQueuedNetConnect(SANetClient&, std::shared_ptr<SANetSocketStream>, std::string, uint16_t, uint64_t);
    bool OnExecute() override;
    void Delete() noexcept override { delete this; }
    virtual ~SATaskQueuedNetConnect() = default;
};
class SATaskQueuedNetTransmit final : public PSTaskAsyncBase {
    SANetClient& client_;
    std::shared_ptr<SANetSocketStream> socket_;
    std::vector<uint8_t> bytes_;
    uint64_t generation_;
public:
    SATaskQueuedNetTransmit(SANetClient&, std::shared_ptr<SANetSocketStream>, std::vector<uint8_t>, uint64_t);
    bool OnExecute() override;
    void Delete() noexcept override { delete this; }
    virtual ~SATaskQueuedNetTransmit() = default;
};
class SATaskLoopedNetReceiver final : public PSTaskAsyncLooped {
    SANetClient& client_;
    std::shared_ptr<SANetSocketStream> socket_;
    uint64_t generation_;
    SATaskLoopedFrameBufferProcessor& processor_;
public:
    SATaskLoopedNetReceiver(SANetClient&, std::shared_ptr<SANetSocketStream>, uint64_t, SATaskLoopedFrameBufferProcessor&);
    bool OnExecute() override;
    bool OnEntry() override;
    void OnExit() noexcept override;
    void RequestExit() noexcept override;
    virtual ~SATaskLoopedNetReceiver() = default;
};
class SANetClient {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<SANetEvent> events_;
    size_t queued_bytes_ = 0;
    std::atomic<bool> out_of_memory_{false};
    std::atomic<bool> quiescing_{true};
    uint64_t generation_ = 0;
    PSTaskQueue transmitter_{1};
    PSThreadTaskLoop receiver_thread_;
    std::unique_ptr<SATaskLoopedNetReceiver> receiver_;
    PSThreadTaskLoop frame_thread_;
    std::unique_ptr<SATaskLoopedFrameBufferProcessor> processor_;
    std::shared_ptr<SANetSocketStream> socket_;
    std::shared_ptr<PSRendererD3D11> renderer_;
    sd_tls_state last_tls_state_{};
    SAConnectionWatcherConfig watcher_config_{};
    PSThreadTaskLoop watcher_thread_;
    std::unique_ptr<SATaskLoopedNetConnectionWatcher> watcher_;
    sd_connection_watch_state last_watch_state_{};
public:
    SANetClient() = default;
    void SetRenderer(std::shared_ptr<PSRendererD3D11> renderer) { renderer_ = std::move(renderer); }
    ~SANetClient();
    void Connect(const std::string&, uint16_t, uint32_t connect_timeout, uint32_t io_timeout, uint32_t transport,
                 const SASecureSocketLayerConfig& tls = {}, const SAConnectionWatcherConfig& watcher = {});
    void TlsStateGet(sd_tls_state& state) const;
    void WatchStateGet(sd_connection_watch_state&) const;
    void Activity();
    void Start(const protocol::IdentificationPacket& identification);
    void Stop() noexcept;
    void PrepareDisconnect() noexcept;
    void FinishDisconnect(uint32_t grace_ms) noexcept;
    bool Quiescing() const noexcept { return quiescing_.load(); }
    void Transmit(std::vector<uint8_t> bytes, bool replace_pending = false);
    template<class Packet> void Transmit(const Packet& packet, bool replace_pending = false) {
        const auto bytes = protocol::bytes(packet);
        Transmit(std::vector<uint8_t>(bytes.begin(), bytes.end()), replace_pending);
    }
    bool WaitForTransmitter(uint32_t timeout_ms) { return transmitter_.WaitForIdle(timeout_ms); }
    bool NextEvent(SANetEvent& out, uint32_t timeout_ms);
    bool Post(SANetEvent event) noexcept;
    void PostFailure(uint64_t generation, sd_result status, const char* text, uint32_t disconnect_reason = 0) noexcept;
};
}
