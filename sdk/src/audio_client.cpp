#include "audio_client.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace sd::native {
SATaskLoopedUsbAudioRenderer::SATaskLoopedUsbAudioRenderer(SANetAudioClient& owner,const sd_audio_config& config,std::wstring endpoint)
    :owner_(owner),config_(config),endpoint_(std::move(endpoint)) { renderer_.Attach(new SAAudioRenderer); }
bool SATaskLoopedUsbAudioRenderer::OnEntry() {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_NORMAL);
    const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);com_=SUCCEEDED(hr);
    try {
        if(FAILED(hr)) throw AudioFailure(hr,"USB audio worker COM initialization");
        if(owner_.Stopping()) return false;
        if(config_.playback) { renderer_->Initialize(endpoint_,config_.activation_timeout_ms);renderer_->Start(); }
        owner_.Ready(config_.playback?renderer_->Channels():0,config_.playback!=0);return !owner_.Stopping();
    } catch(const AudioFailure& e) { owner_.Failed(e.status,e.what(),e.hresult); }
      catch(const Failure& e) { owner_.Failed(e.status,e.what()); }
      catch(...) { owner_.Failed(SD_INTERNAL_ERROR,"USB audio initialization failed"); }
    return false;
}
bool SATaskLoopedUsbAudioRenderer::OnExecute() {
    try {
        SANetPacket packet;if(!packets_.Take(packet) || owner_.Stopping()) return false;
        const auto sequence=owner_.PacketReceived();
        if(config_.playback) renderer_->Render(packet.payload);
        owner_.Push(packet.payload,sequence);return !owner_.Stopping();
    } catch(const AudioFailure& e) { owner_.Failed(e.status,e.what(),e.hresult); }
      catch(const Failure& e) { owner_.Failed(e.status,e.what()); }
      catch(...) { owner_.Failed(SD_INTERNAL_ERROR,"USB audio rendering failed"); }
    return false;
}
void SATaskLoopedUsbAudioRenderer::OnExit() noexcept {
    packets_.Close();renderer_->Stop();owner_.Closed();if(com_) CoUninitialize();com_=false;
}
void SATaskLoopedUsbAudioRenderer::RequestExit() noexcept { packets_.Close();renderer_->Cancel(); }
SATaskLoopedNetAudioReceiverRenderer::SATaskLoopedNetAudioReceiverRenderer(SANetAudioClient& owner,
    std::shared_ptr<SANetSocketStream> socket,const sd_audio_config& config,std::wstring endpoint)
    :owner_(owner),socket_(std::move(socket)),config_(config),endpoint_(std::move(endpoint)) {
    renderer_.Attach(new SAAudioRenderer);
}
bool SATaskLoopedNetAudioReceiverRenderer::OnEntry() {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_NORMAL);
    const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);com_=SUCCEEDED(hr);
    try {
        if (FAILED(hr)) throw AudioFailure(hr,"Audio worker COM initialization");
        if (owner_.Stopping()) return false;
        if (config_.playback) { renderer_->Initialize(endpoint_,config_.activation_timeout_ms);renderer_->Start(); }
        owner_.Ready(config_.playback?renderer_->Channels():0,config_.playback!=0);
        return !owner_.Stopping();
    } catch(const AudioFailure& e) { owner_.Failed(e.status,e.what(),e.hresult); }
      catch(const Failure& e) { owner_.Failed(e.status,e.what()); }
      catch(...) { owner_.Failed(SD_INTERNAL_ERROR,"Audio receiver initialization failed"); }
    return false;
}
bool SATaskLoopedNetAudioReceiverRenderer::OnExecute() {
    try {
        SANetPacket packet;if (!socket_->ReceiveTcpStream(packet)) return false;
        if (owner_.Stopping()) return true; // bounded half-close drain, with no output/capture
        const auto sequence=owner_.PacketReceived();
        const auto type=protocol::decode<protocol::PacketHeader>(packet.header).prefix.type;
        if (type==protocol::HeaderType::DISCONNECT) {
            owner_.ServerDisconnected(uint32_t(protocol::decode<protocol::ServerDisconnectHeader>(packet.header).reason));
            try { const auto bytes=protocol::bytes(protocol::ClientDisconnectHeader{});socket_->Send({bytes.begin(),bytes.end()}); } catch(...) {}
            return false;
        }
        if (type==protocol::HeaderType::AUDIO && !packet.payload.empty()) {
            if (config_.playback) renderer_->Render(packet.payload);
            owner_.Push(packet.payload,sequence);
        }
        return true;
    } catch(const AudioFailure& e) { owner_.Failed(e.status,e.what(),e.hresult); }
      catch(const Failure& e) { owner_.Failed(e.status,e.what()); }
      catch(const std::bad_alloc&) { owner_.Failed(SD_OUT_OF_MEMORY,"Audio receiver allocation failed"); }
      catch(...) { owner_.Failed(SD_INTERNAL_ERROR,"Audio receiver failed"); }
    return false;
}
void SATaskLoopedNetAudioReceiverRenderer::OnExit() noexcept {
    socket_->Close();renderer_->Stop();owner_.Closed();if (com_) CoUninitialize();com_=false;
}
void SATaskLoopedNetAudioReceiverRenderer::RequestExit() noexcept { renderer_->Cancel();socket_->Close(); }
SATaskQueuedNetAudioConnect::SATaskQueuedNetAudioConnect(SANetAudioClient& owner,std::shared_ptr<SANetSocketStream> socket,
    std::string address,uint16_t port,const protocol::IdentificationData& identity,bool probe,uint32_t timeout)
    :owner_(owner),socket_(std::move(socket)),address_(std::move(address)),port_(port),probe_(probe),probe_timeout_(timeout) {
    identity_.identity=identity;
}
bool SATaskQueuedNetAudioConnect::OnExecute() {
    try {
        if (probe_) {
            sd_host host{};host.struct_size=sizeof(host);uint32_t count=0;
            const auto result=sd_discover(address_.c_str(),port_,probe_timeout_,&host,1,&count);
            if (result==SD_OK && count && host.protocol_major) {
                { std::lock_guard<std::mutex> lock(owner_.mutex_);owner_.state_.server_features=host.feature_flags; }
                if (!(host.feature_flags&SD_SERVER_FEATURE_AUDIO)) throw Failure(SD_UNSUPPORTED,"Server does not advertise audio capability");
            }
        }
        if (owner_.Stopping()) return false;
        socket_->TcpCreateConnect(address_,port_);
        const auto wire=protocol::bytes(identity_);socket_->Send({wire.begin(),wire.end()});
        if (owner_.Stopping()) return false;
        owner_.StartReceiver();return true;
    } catch(const Failure& e) { owner_.Failed(e.status,e.what()); }
      catch(const std::bad_alloc&) { owner_.Failed(SD_OUT_OF_MEMORY,"Audio connect allocation failed"); }
      catch(...) { owner_.Failed(SD_INTERNAL_ERROR,"Audio connect task failed"); }
    socket_->Close();return false;
}
sd_result SANetAudioClient::Start(const std::string& address,uint16_t port,const protocol::IdentificationData& identity,
    const sd_audio_config& config,const std::wstring& endpoint,uint32_t connect_timeout,uint32_t io_timeout,const SASecureSocketLayerConfig& tls) noexcept {
    Stop();
    try {
        stopping_=false;connected_=false;io_timeout_=io_timeout;
        { std::lock_guard<std::mutex> lock(mutex_);state_={};state_.struct_size=sizeof(state_);state_.state=SD_AUDIO_CONNECTING;
          disconnect_state_={sizeof(disconnect_state_),0,0,0};
          state_.server_features=UINT32_MAX;error_.clear();capacity_frames_=uint64_t(config.capture_queue_ms)*48; }
        last_tls_state_={};
        socket_=std::make_shared<SANetSocketStream>(connect_timeout,io_timeout,SD_TRANSPORT_TCP,true,tls);
        receiver_=std::make_unique<SATaskLoopedNetAudioReceiverRenderer>(*this,socket_,config,endpoint);
        if (!transmitter_.Start()) throw Failure(SD_INTERNAL_ERROR,"Cannot start audio task queue");
        if (!transmitter_.TaskSubmit(new SATaskQueuedNetAudioConnect(*this,socket_,address,port,identity,config.probe_server!=0,
                                                                   std::min<uint32_t>(connect_timeout,500))))
            throw Failure(SD_INTERNAL_ERROR,"Cannot queue audio connect");
        return SD_PENDING;
    } catch(const Failure& e) { Failed(e.status,e.what());return e.status; }
      catch(const std::bad_alloc&) { Failed(SD_OUT_OF_MEMORY,"Cannot allocate audio session");return SD_OUT_OF_MEMORY; }
      catch(...) { Failed(SD_INTERNAL_ERROR,"Cannot create audio session");return SD_INTERNAL_ERROR; }
}
void SANetAudioClient::Stop() noexcept {
    stopping_=true;changed_.notify_all();
    if (receiver_) receiver_->CancelRenderer();
    if (usb_receiver_) usb_receiver_->RequestExit();
    if (socket_) {
        if (connected_) {
            try { const auto data=protocol::bytes(protocol::ClientDisconnectHeader{});socket_->Send({data.begin(),data.end()}); } catch(...) {}
            socket_->ShutdownSend(std::min<uint32_t>(100,io_timeout_));
        } else socket_->Close();
    }
    transmitter_.Stop(false); // joins a connect task before it can start the RX loop late
    receiver_thread_.Wait();
    if (socket_) socket_->Close();
    if (socket_) socket_->TlsStateGet(last_tls_state_);
    receiver_.reset();usb_receiver_.reset();socket_.reset();connected_=false;
    { std::lock_guard<std::mutex> lock(mutex_);queue_.clear();delivered_.reset();queue_frames_=0;state_.state=SD_AUDIO_DISCONNECTED;state_.playback_started=0; }
    changed_.notify_all();
}
sd_result SANetAudioClient::StartUsb(const sd_audio_config& config,const std::wstring& endpoint) noexcept {
    Stop();
    try {
        stopping_=false;last_tls_state_={};
        { std::lock_guard<std::mutex> lock(mutex_);state_={};state_.struct_size=sizeof(state_);state_.state=SD_AUDIO_CONNECTING;
          disconnect_state_={sizeof(disconnect_state_),0,0,0};
          state_.server_features=UINT32_MAX;error_.clear();capacity_frames_=uint64_t(config.capture_queue_ms)*48; }
        usb_receiver_=std::make_unique<SATaskLoopedUsbAudioRenderer>(*this,config,endpoint);
        if(!receiver_thread_.Start(*usb_receiver_)) throw Failure(SD_INTERNAL_ERROR,"Cannot start USB audio renderer");
        return SD_PENDING;
    } catch(const Failure& e) { Failed(e.status,e.what());return e.status; }
      catch(const std::bad_alloc&) { Failed(SD_OUT_OF_MEMORY,"Cannot allocate USB audio renderer");return SD_OUT_OF_MEMORY; }
      catch(...) { Failed(SD_INTERNAL_ERROR,"Cannot create USB audio renderer");return SD_INTERNAL_ERROR; }
}
bool SANetAudioClient::SubmitUsb(SANetPacket packet) noexcept {
    if(stopping_ || !usb_receiver_) return false;
    { std::lock_guard<std::mutex> lock(mutex_);if(state_.state==SD_AUDIO_FAILED) return false; }
    try { return usb_receiver_->Submit(std::move(packet)); }
    catch(const Failure& e) { Failed(e.status,e.what()); }
    catch(...) { Failed(SD_OUT_OF_MEMORY,"USB audio input queue allocation failed"); }
    usb_receiver_->RequestExit();return false;
}
void SANetAudioClient::TlsStateGet(sd_tls_state& state) const {
    if(socket_) socket_->TlsStateGet(state);
    else { state=last_tls_state_;state.struct_size=sizeof(state);state.connected=0; }
}
void SANetAudioClient::StartReceiver() {
    if (stopping_) return;
    connected_=true;
    if (!receiver_thread_.Start(*receiver_)) throw Failure(SD_INTERNAL_ERROR,"Cannot start audio receiver loop");
}
void SANetAudioClient::Ready(uint32_t channels,bool playback) {
    { std::lock_guard<std::mutex> lock(mutex_);
      // USB may overflow its input queue while endpoint activation is still
      // completing. Late activation must not revive an already failed session.
      if(stopping_ || state_.state==SD_AUDIO_FAILED) return;
      state_.state=SD_AUDIO_STREAMING;state_.output_channels=channels;state_.playback_started=playback; }
    changed_.notify_all();
}
void SANetAudioClient::Failed(sd_result status,const char* text,HRESULT hr) noexcept {
    if (stopping_) return;
    try { std::lock_guard<std::mutex> lock(mutex_);state_.state=SD_AUDIO_FAILED;state_.last_result=status;state_.last_hresult=hr;error_=text; }
    catch(...) {}
    changed_.notify_all();
}
void SANetAudioClient::Closed() noexcept {
    connected_=false;
    { std::lock_guard<std::mutex> lock(mutex_);if (state_.state!=SD_AUDIO_FAILED) state_.state=SD_AUDIO_DISCONNECTED;state_.playback_started=0; }
    changed_.notify_all();
}
void SANetAudioClient::ServerDisconnected(uint32_t reason) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!stopping_) disconnect_state_={sizeof(disconnect_state_),1,reason,0};
}
void SANetAudioClient::DisconnectStateGet(sd_audio_disconnect_state& state) const {
    std::lock_guard<std::mutex> lock(mutex_);state=disconnect_state_;
}
void SANetAudioClient::ClearDisconnectState() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);disconnect_state_={sizeof(disconnect_state_),0,0,0};
}
uint64_t SANetAudioClient::PacketReceived() { std::lock_guard<std::mutex> lock(mutex_);return ++state_.packets_received; }
void SANetAudioClient::Push(const std::vector<uint8_t>& pcm,uint64_t sequence) {
    const uint64_t frames=pcm.size()/4;uint64_t base;
    { std::lock_guard<std::mutex> lock(mutex_);base=state_.frames_received;state_.frames_received+=frames; }
    if (!capacity_frames_) return;
    uint64_t offset=0;
    while (offset<frames && !stopping_) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock,[this]{return stopping_||state_.state==SD_AUDIO_FAILED||queue_frames_<capacity_frames_;});
        if (stopping_ || state_.state==SD_AUDIO_FAILED) return;
        const auto count=std::min(frames-offset,capacity_frames_-queue_frames_);
        auto block=std::make_shared<Block>();block->sequence=sequence;block->frame_index=base+offset;block->samples.resize(size_t(count)*2);
        std::memcpy(block->samples.data(),pcm.data()+size_t(offset)*4,size_t(count)*4);
        queue_.push_back(std::move(block));queue_frames_+=count;offset+=count;lock.unlock();changed_.notify_all();
    }
}
void SANetAudioClient::StateGet(sd_audio_state& state) const {
    std::lock_guard<std::mutex> lock(mutex_);state=state_;state.struct_size=sizeof(state);state.buffered_frames=queue_frames_;
}
std::string SANetAudioClient::LastError() const { std::lock_guard<std::mutex> lock(mutex_);return error_; }
sd_result SANetAudioClient::Poll(uint32_t timeout,sd_audio_buffer& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock,std::chrono::milliseconds(timeout),[this]{return !queue_.empty()||state_.state==SD_AUDIO_FAILED||state_.state==SD_AUDIO_DISCONNECTED;});
    if (queue_.empty()) {
        if (state_.state==SD_AUDIO_FAILED) return state_.last_result;
        return state_.state==SD_AUDIO_DISCONNECTED?SD_DISCONNECTED:SD_TIMEOUT;
    }
    delivered_=std::move(queue_.front());queue_.pop_front();queue_frames_-=delivered_->samples.size()/2;
    out={};out.struct_size=sizeof(out);out.sample_rate=48000;out.channels=2;out.sample_format=SD_AUDIO_S16_LE;
    out.frame_count=uint32_t(delivered_->samples.size()/2);out.sequence=delivered_->sequence;out.frame_index=delivered_->frame_index;
    out.samples=delivered_->samples.data();lock.unlock();changed_.notify_all();return SD_OK;
}
}
