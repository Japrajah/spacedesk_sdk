#include "net_client.h"
#include "websocket.h"
#include "renderer_d3d11.h"
#include <algorithm>
#include <chrono>
#include <limits>

namespace sd::native {
SANetSocketStream::~SANetSocketStream() {
    Close();
    const auto socket = socket_.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) closesocket(socket);
}
void SANetSocketStream::TlsStateGet(sd_tls_state& state) const {
    state={};state.struct_size=sizeof(state);
    if(tls_) tls_->StateGet(state);
    if(closing_) state.connected=0;
}
void SANetSocketStream::Close() noexcept {
    // Keep the handle allocated until both tasks release their shared ownership;
    // shutdown wakes a blocked reader without a Windows socket-handle reuse race.
    if (!closing_.exchange(true)) {
        const auto socket = socket_.load();
        if (socket != INVALID_SOCKET) shutdown(socket, SD_BOTH);
    }
}
void SANetSocketStream::ShutdownSend(uint32_t grace_ms) noexcept {
    if (!closing_) {
        if (transport_ == SD_TRANSPORT_WEBSOCKET) {
            try { WebSocketControlSend(8, {3, 232}); } catch (...) {} // RFC close 1000 after native disconnect.
        }
        if(tls_) {
            try {
                const auto deadline=GetTickCount64()+grace_ms;
                for(;;) {
                    const auto progress=tls_->Shutdown();
                    if(progress==SATlsProgress::Complete || progress==SATlsProgress::Closed || GetTickCount64()>=deadline) break;
                    Wait(progress==SATlsProgress::Write,deadline);
                }
            } catch(...) {} // Close/Stop must still cancel a failed or stalled TLS stream.
        }
        // Preserve the queued disconnect and FIN while the reader drains late
        // display bytes. Closing with unread receive data can otherwise send RST.
        drain_until_ = GetTickCount64() + grace_ms;
        const auto socket = socket_.load();
        if (socket != INVALID_SOCKET) shutdown(socket, SD_SEND);
    }
}
bool SANetSocketStream::Wait(bool write, ULONGLONG deadline) {
    if (closing_) throw Failure(SD_IO_ERROR, "Connection was cancelled");
    const SOCKET socket = socket_.load();
    fd_set ready, errors; FD_ZERO(&ready); FD_SET(socket, &ready);
    FD_ZERO(&errors); FD_SET(socket, &errors);
    const auto now = GetTickCount64();
    const auto delay = deadline > now ? std::min<ULONGLONG>(deadline - now, 100) : 0;
    timeval timeout{long(delay / 1000), long((delay % 1000) * 1000)};
    int result = select(0, write ? nullptr : &ready, write ? &ready : nullptr, &errors, &timeout);
    if (result == SOCKET_ERROR || FD_ISSET(socket, &errors)) throw Failure(SD_IO_ERROR, "TCP socket error");
    return result > 0;
}
void SANetSocketStream::TcpCreateConnect(const std::string& ip, uint16_t port) {
    const auto connection_deadline=GetTickCount64()+connect_timeout_;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port ? port : protocol::default_port);
    if (InetPtonA(AF_INET, ip.c_str(), &address.sin_addr) != 1) throw Failure(SD_INVALID_ARGUMENT, "IPv4 literal required");
    if (closing_) throw Failure(SD_IO_ERROR, "Connection was cancelled");
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) throw Failure(SD_IO_ERROR, "Cannot create TCP socket");
    socket_ = socket;
    if (closing_) throw Failure(SD_IO_ERROR, "Connection was cancelled");
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking)) throw Failure(SD_IO_ERROR, "Cannot set TCP nonblocking mode");
    BOOL enabled = TRUE;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)))
        throw Failure(SD_IO_ERROR, "Cannot configure TCP_NODELAY");
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) throw Failure(SD_IO_ERROR, "TCP connect failed");
        const auto deadline = connection_deadline;
        while (!Wait(true, deadline)) {
            if (GetTickCount64() >= deadline) throw Failure(SD_IO_ERROR, "TCP connect timed out");
        }
        int error = 0, length = sizeof(error);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) || error)
            throw Failure(SD_IO_ERROR, "TCP connection refused");
    }
    if(tls_) SslConnect(connection_deadline);
    if (transport_ == SD_TRANSPORT_WEBSOCKET) WebSocketConnect(ip, port ? port : protocol::default_port);
}
void SANetSocketStream::SslConnect(ULONGLONG deadline) {
    tls_->Initialize(socket_.load());
    for(;;) {
        if(closing_) throw Failure(SD_IO_ERROR,"TLS connection was cancelled");
        if(GetTickCount64()>=deadline) throw Failure(SD_TLS_ERROR,"TLS handshake timed out");
        const auto progress=tls_->Connect();
        if(progress==SATlsProgress::Complete) return;
        if(progress==SATlsProgress::Closed) throw Failure(SD_TLS_ERROR,"TLS peer closed during handshake");
        Wait(progress==SATlsProgress::Write,deadline);
    }
}
void SANetSocketStream::Send(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (websocket_close_sent_) throw Failure(SD_IO_ERROR, "WebSocket is closing");
    if (transport_ == SD_TRANSPORT_WEBSOCKET) SendBytes(SNProtocolWebSocket::ClientFrameCreate(2, bytes));
    else SendBytes(bytes);
}
void SANetSocketStream::SendBytes(const std::vector<uint8_t>& bytes) {
    const auto deadline = GetTickCount64() + io_timeout_;
    size_t sent = 0;
    while (sent < bytes.size()) {
        if (closing_) throw Failure(SD_IO_ERROR, "Connection was closed during send");
        if (GetTickCount64() >= deadline) throw Failure(SD_IO_ERROR, "TCP send timed out");
        if(tls_) {
            const auto result=tls_->Write(bytes.data()+sent,std::min<size_t>(bytes.size()-sent,1024*1024));
            if(result.count) { sent+=result.count;continue; }
            if(result.progress==SATlsProgress::Closed) throw Failure(SD_TLS_ERROR,"TLS peer closed during send");
            Wait(result.progress==SATlsProgress::Write,deadline);continue;
        }
        int count = send(socket_.load(), reinterpret_cast<const char*>(bytes.data() + sent),
            int(std::min<size_t>(bytes.size() - sent, 1024 * 1024)), 0);
        if (count > 0) { sent += size_t(count); continue; }
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) { Wait(true, deadline); continue; }
        throw Failure(SD_IO_ERROR, "TCP send failed");
    }
}
void SANetSocketStream::WebSocketConnect(const std::string& host, uint16_t port) {
    const auto key = SNProtocolWebSocket::ClientKeyCreate();
    const auto request = SNProtocolWebSocket::HandshakeRequestCreate(host, port, key);
    SendBytes(std::vector<uint8_t>(request.begin(), request.end()));
    std::string response;
    ULONGLONG started = GetTickCount64(); // Also bounds a peer that never answers HTTP.
    while (response.size() < 16384) {
        uint8_t byte;
        if (!ReceiveExact(&byte, 1, started)) throw Failure(SD_IO_ERROR, "WebSocket upgrade was closed");
        response += char(byte);
        if (response.size() >= 4 && response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) {
            SNProtocolWebSocket::HandshakeResponseValidate(response, key);
            return;
        }
    }
    throw Failure(SD_PROTOCOL_ERROR, "WebSocket HTTP response exceeds 16 KiB");
}
void SANetSocketStream::WebSocketControlSend(uint8_t opcode, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (websocket_close_sent_) return;
    if (opcode == 8) websocket_close_sent_ = true;
    SendBytes(SNProtocolWebSocket::ClientFrameCreate(opcode, payload));
}
bool SANetSocketStream::WebSocketMessageReceive(std::vector<uint8_t>& message) {
    bool fragmented = false;
    ULONGLONG started = 0;
    message.clear();
    for (;;) {
        std::array<uint8_t, 2> prefix{};
        if (!ReceiveExact(prefix.data(), prefix.size(), started)) return false;
        const bool final = (prefix[0] & 0x80) != 0;
        const uint8_t opcode = prefix[0] & 15, short_length = prefix[1] & 127;
        const bool control = opcode >= 8;
        if ((prefix[0] & 0x70) || (prefix[1] & 0x80))
            throw Failure(SD_PROTOCOL_ERROR, "WebSocket server sent reserved bits or a masked frame");
        if ((opcode != 0 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10) ||
            (control && (!final || short_length > 125)))
            throw Failure(SD_PROTOCOL_ERROR, "Invalid WebSocket opcode or control frame");
        uint64_t length = short_length;
        if (short_length >= 126) {
            std::array<uint8_t, 8> extended{};
            const size_t count = short_length == 126 ? 2 : 8;
            if (!ReceiveExact(extended.data(), count, started)) return false;
            length = 0;
            for (size_t i = 0; i < count; ++i) length = (length << 8) | extended[i];
            // Native SNProtocolWebSocket::WebSocketHeaderInitialize uses the
            // 64-bit form at exactly 65535 too. Retain that proven boundary quirk.
            if ((short_length == 126 && length < 126) || (short_length == 127 && length < 65535))
                throw Failure(SD_PROTOCOL_ERROR, "Nonminimal WebSocket payload length");
        }
        if (length > uint64_t(max_payload) + protocol::header_size ||
            (!control && length + message.size() > uint64_t(max_payload) + protocol::header_size))
            throw Failure(SD_PROTOCOL_ERROR, "WebSocket message exceeds payload limit");
        if (control) {
            std::vector<uint8_t> payload(static_cast<size_t>(length));
            if (length && !ReceiveExact(payload.data(), payload.size(), started)) return false;
            if (opcode == 8) {
                SNProtocolWebSocket::ClosePayloadValidate(payload);
                WebSocketControlSend(8, payload);
                return false;
            }
            if (opcode == 9) WebSocketControlSend(10, payload);
            if (!fragmented) started = 0;
            continue;
        }
        if ((opcode == 0) != fragmented)
            throw Failure(SD_PROTOCOL_ERROR, "Unexpected WebSocket continuation/data frame");
        const size_t offset = message.size();
        message.resize(offset + static_cast<size_t>(length));
        if (length && !ReceiveExact(message.data() + offset, static_cast<size_t>(length), started)) return false;
        if (final) return true;
        fragmented = true;
    }
}
bool SANetSocketStream::ReceiveExact(uint8_t* output, size_t length, ULONGLONG& started) {
    size_t count = 0;
    while (count < length) {
        if (closing_) return false;
        const auto now = GetTickCount64();
        const auto drain_deadline = drain_until_.load();
        if (drain_deadline && now >= drain_deadline) return false;
        if (started && now - started >= io_timeout_) throw Failure(SD_IO_ERROR, "Incomplete TCP packet timed out");
        if(tls_) {
            const auto incomplete=tls_->IncompleteRecordSince();
            if(incomplete && now-incomplete>=io_timeout_) throw Failure(SD_IO_ERROR,"Incomplete TLS record timed out");
            const auto result=tls_->Read(output+count,std::min<size_t>(length-count,1024*1024));
            if(result.count) { if(!started) started=GetTickCount64();count+=result.count;continue; }
            if(result.progress==SATlsProgress::Closed) {
                if(started) throw Failure(SD_PROTOCOL_ERROR,"TLS peer closed in the middle of a packet");
                return false;
            }
            auto deadline=started?started+io_timeout_:now+100;
            const auto encrypted_since=tls_->IncompleteRecordSince();
            if(encrypted_since) deadline=std::min(deadline,encrypted_since+io_timeout_);
            Wait(result.progress==SATlsProgress::Write,drain_deadline?std::min(deadline,drain_deadline):deadline);continue;
        }
        const int received = recv(socket_.load(), reinterpret_cast<char*>(output + count),
            int(std::min<size_t>(length - count, 1024 * 1024)), 0);
        if (received > 0) { if (!started) started = GetTickCount64(); count += size_t(received); continue; }
        if (!received) {
            if (started) throw Failure(SD_PROTOCOL_ERROR, "Peer closed in the middle of a packet");
            return false;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            if (closing_) return false;
            throw Failure(SD_IO_ERROR, "TCP receive failed");
        }
        const auto deadline = started ? started + io_timeout_ : now + 100;
        Wait(false, drain_deadline ? std::min(deadline, drain_deadline) : deadline);
    }
    return true;
}
bool SANetSocketStream::ReceiveTcpStream(SANetPacket& packet) {
    ULONGLONG started = 0;
    std::vector<uint8_t> message;
    if (transport_ == SD_TRANSPORT_WEBSOCKET) {
        if (!WebSocketMessageReceive(message)) return false;
        if (message.size() < packet.header.size()) throw Failure(SD_PROTOCOL_ERROR, "Truncated spacedesk WebSocket header");
        std::copy_n(message.begin(), packet.header.size(), packet.header.begin());
    } else if (!ReceiveExact(packet.header.data(), packet.header.size(), started)) return false;
    const auto header = protocol::decode<protocol::PacketHeader>(packet.header);
    const auto type = header.prefix.type;
    const auto length = header.prefix.payload_length;
    if (audio_) {
        if ((type == protocol::HeaderType::AUDIO && (length > 4 * 1024 * 1024 || length % 4)) ||
            (type != protocol::HeaderType::AUDIO && (length || (type != protocol::HeaderType::DISCONNECT && type != protocol::HeaderType::PING))))
            throw Failure(SD_PROTOCOL_ERROR, "Invalid audio packet type or PCM length");
    } else if (length > max_payload || (type != protocol::HeaderType::FRAMEBUFFER && length) ||
               (type == protocol::HeaderType::FRAMEBUFFER && !length))
        throw Failure(SD_PROTOCOL_ERROR, "Invalid packet payload length");
    if (transport_ == SD_TRANSPORT_WEBSOCKET) {
        if (message.size() != packet.header.size() + length)
            throw Failure(SD_PROTOCOL_ERROR, "WebSocket message does not match spacedesk packet length");
        packet.payload.assign(message.begin() + packet.header.size(), message.end());
    } else {
        packet.payload.resize(length);
        if (length && !ReceiveExact(packet.payload.data(), length, started)) return false;
    }
    return true;
}
SATaskQueuedNetConnect::SATaskQueuedNetConnect(SANetClient& c, std::shared_ptr<SANetSocketStream> s,
    std::string address, uint16_t port, uint64_t generation)
    : client_(c), socket_(std::move(s)), address_(std::move(address)), port_(port), generation_(generation) {}
bool SATaskQueuedNetConnect::OnExecute() {
    try {
        socket_->TcpCreateConnect(address_, port_);
        client_.Post({SANetEvent::Kind::Connected, generation_}); return true;
    } catch (const Failure& e) { client_.PostFailure(generation_, e.status, e.what()); }
      catch (...) { client_.PostFailure(generation_, SD_INTERNAL_ERROR, "Connect task failed"); }
    return false;
}
SATaskQueuedNetTransmit::SATaskQueuedNetTransmit(SANetClient& c, std::shared_ptr<SANetSocketStream> s,
    std::vector<uint8_t> bytes, uint64_t generation)
    : client_(c), socket_(std::move(s)), bytes_(std::move(bytes)), generation_(generation) {}
bool SATaskQueuedNetTransmit::OnExecute() {
    try { socket_->Send(bytes_); return true; }
    catch (const Failure& e) { client_.PostFailure(generation_, e.status, e.what()); }
    catch (...) { client_.PostFailure(generation_, SD_INTERNAL_ERROR, "Transmit task failed"); }
    return false;
}
SATaskLoopedNetReceiver::SATaskLoopedNetReceiver(SANetClient& c, std::shared_ptr<SANetSocketStream> s, uint64_t generation,
    SATaskLoopedFrameBufferProcessor& processor)
    : client_(c), socket_(std::move(s)), generation_(generation), processor_(processor) {}
bool SATaskLoopedNetReceiver::OnEntry() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    return true;
}
bool SATaskLoopedNetReceiver::OnExecute() {
    try {
        SANetEvent event{SANetEvent::Kind::Packet, generation_};
        if (!socket_->ReceiveTcpStream(event.packet)) return false;
        const auto type = protocol::decode<protocol::PacketHeader>(event.packet.header).prefix.type;
        if(type==protocol::HeaderType::FRAMEBUFFER || type==protocol::HeaderType::VISIBILITY || type==protocol::HeaderType::PING)
            client_.Activity();
        if (type == protocol::HeaderType::FRAMEBUFFER) {
            if (client_.Quiescing()) return true;
            return processor_.Submit(std::move(event.packet)) || client_.Quiescing();
        }
        if (type == protocol::HeaderType::PING && !client_.Quiescing()) {
            if (!processor_.Ping()) return client_.Quiescing();
            if (protocol::decode<protocol::PingHeader>(event.packet.header).option == protocol::PingOption::ECHO_REQUEST)
                client_.Transmit(protocol::PingHeader{protocol::PingOption::ECHO_REPLY});
        }
        return client_.Post(std::move(event));
    } catch (const Failure& e) { client_.PostFailure(generation_, e.status, e.what()); }
      catch (const std::bad_alloc&) { client_.PostFailure(generation_, SD_OUT_OF_MEMORY, "Receive allocation failed"); }
      catch (...) { client_.PostFailure(generation_, SD_INTERNAL_ERROR, "Receive task failed"); }
    return false;
}
void SATaskLoopedNetReceiver::OnExit() noexcept {
    socket_->Close(); client_.Post({SANetEvent::Kind::Disconnected, generation_});
}
void SATaskLoopedNetReceiver::RequestExit() noexcept { socket_->Close(); }
SANetClient::~SANetClient() { Stop(); }
void SANetClient::Connect(const std::string& ip, uint16_t port, uint32_t connect_timeout, uint32_t io_timeout, uint32_t transport,
                          const SASecureSocketLayerConfig& tls, const SAConnectionWatcherConfig& watcher) {
    Stop();
    last_tls_state_={};
    watcher_config_=watcher;last_watch_state_={};
    socket_ = std::make_shared<SANetSocketStream>(connect_timeout, io_timeout, transport,false,tls);
    if (!transmitter_.Start()) throw Failure(SD_INTERNAL_ERROR, "Cannot start transmit task queue");
    if (!transmitter_.TaskSubmit(new SATaskQueuedNetConnect(*this, socket_, ip, port, generation_)))
        throw Failure(SD_INTERNAL_ERROR, "Cannot queue TCP connect task");
}
void SANetClient::TlsStateGet(sd_tls_state& state) const {
    if(socket_) socket_->TlsStateGet(state);
    else { state=last_tls_state_;state.struct_size=sizeof(state);state.connected=0; }
}
void SANetClient::Activity() { if(watcher_) watcher_->Activity(); }
void SANetClient::WatchStateGet(sd_connection_watch_state& state) const {
    if(watcher_) watcher_->StateGet(state);
    else { state=last_watch_state_;state.struct_size=sizeof(state);state.running=0; }
    state.enabled=watcher_config_.period_ms!=0;
}
void SANetClient::Start(const protocol::IdentificationPacket& identification) {
    if (!socket_) throw Failure(SD_IO_ERROR, "No TCP stream to start");
    const auto generation = generation_;
    processor_ = std::make_unique<SATaskLoopedFrameBufferProcessor>(
        [this, generation](std::shared_ptr<const FrameSnapshot> frame, const std::vector<protocol::Rectangle>& dirty) {
            if (quiescing_) return true;
            if (renderer_) {
                try { renderer_->OnFrame(frame, dirty); }
                catch (const GpuFailure& e) {
                    renderer_->MarkFailed(e.hresult);
                    SANetEvent error{SANetEvent::Kind::RendererError, generation};
                    error.status = e.hresult; error.diagnostic = e.what(); Post(std::move(error));
                }
            }
            SANetEvent event{SANetEvent::Kind::Frame, generation}; event.frame = std::move(frame);
            return Post(std::move(event));
        },
        [this](uint32_t count) {
            if (!quiescing_) for (uint32_t i = 0; i < count; ++i) Transmit(protocol::FlowControlAckHeader{});
        },
        [this, generation](sd_result status, const char* text, uint32_t reason) { PostFailure(generation, status, text, reason); });
    quiescing_ = false;
    if(watcher_config_.period_ms) {
        watcher_=std::make_unique<SATaskLoopedNetConnectionWatcher>(watcher_config_,
            [this,generation,socket=socket_](SATaskLoopedNetConnectionWatcher::Decision decision,int32_t elapsed) {
                if(Quiescing()) return;
                if(decision==SATaskLoopedNetConnectionWatcher::Decision::Warn) {
                    SANetEvent event{SANetEvent::Kind::ConnectionWarning,generation};event.status=elapsed;Post(std::move(event));
                } else {
                    PostFailure(generation,SD_IO_ERROR,"Connection watcher timed out");socket->Close();
                }
            });
        if(!watcher_thread_.Start(*watcher_)) throw Failure(SD_INTERNAL_ERROR,"Cannot start connection watcher");
    }
    if (!frame_thread_.Start(*processor_)) throw Failure(SD_INTERNAL_ERROR, "Cannot start framebuffer processor");
    receiver_ = std::make_unique<SATaskLoopedNetReceiver>(*this, socket_, generation_, *processor_);
    if (!receiver_thread_.Start(*receiver_)) throw Failure(SD_INTERNAL_ERROR, "Cannot start receive task");
    Transmit(identification);
}
void SANetClient::Stop() noexcept {
    quiescing_ = true;
    if (socket_) socket_->Close();
    receiver_thread_.Stop();
    watcher_thread_.Stop();
    frame_thread_.Stop();
    transmitter_.Stop(false);
    if(socket_) socket_->TlsStateGet(last_tls_state_);
    if(watcher_) watcher_->StateGet(last_watch_state_);
    watcher_.reset();
    receiver_.reset(); processor_.reset(); socket_.reset();
    if (renderer_) renderer_->ResetStream();
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_; events_.clear(); queued_bytes_ = 0; out_of_memory_ = false;
}
void SANetClient::PrepareDisconnect() noexcept {
    // Stop frame/ACK production before submitting priority disconnect, while
    // the receiver keeps TCP open and discards late display packets.
    quiescing_ = true;
    frame_thread_.Stop();
}
void SANetClient::FinishDisconnect(uint32_t grace_ms) noexcept {
    if (socket_) socket_->ShutdownSend(grace_ms);
    receiver_thread_.Wait();
}
void SANetClient::Transmit(std::vector<uint8_t> bytes, bool replace_pending) {
    if (!socket_) throw Failure(SD_IO_ERROR, "Client is not connected");
    if (!transmitter_.TaskSubmit(new SATaskQueuedNetTransmit(*this, socket_, std::move(bytes), generation_), replace_pending))
        throw Failure(SD_IO_ERROR, "Transmit queue is stopped");
}
bool SANetClient::Post(SANetEvent event) noexcept {
    bool posted = false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto bytes = event.packet.payload.size() + (event.frame ? event.frame->pixels.size() : 0);
        if (event.kind == SANetEvent::Kind::Frame) {
            // The renderer has already presented/ACKed. Keep only the newest
            // unread snapshot; remove older frame events without dropping controls.
            for (auto it = events_.begin(); it != events_.end();) {
                if (it->kind == SANetEvent::Kind::Frame && it->generation == event.generation) {
                    queued_bytes_ -= it->frame->pixels.size(); it = events_.erase(it);
                } else ++it;
            }
        }
        if(event.kind==SANetEvent::Kind::ConnectionWarning) {
            for(auto it=events_.begin();it!=events_.end();) {
                if(it->kind==event.kind && it->generation==event.generation) it=events_.erase(it);
                else ++it;
            }
        }
        if (events_.size() >= 1024 || queued_bytes_ + bytes > 2ull * max_payload) out_of_memory_ = true;
        else { events_.push_back(std::move(event)); queued_bytes_ += bytes; posted = true; }
    } catch (...) { out_of_memory_ = true; }
    changed_.notify_all();
    return posted;
}
void SANetClient::PostFailure(uint64_t generation, sd_result status, const char* text, uint32_t reason) noexcept {
    try { SANetEvent event{SANetEvent::Kind::Error, generation}; event.status = status; event.disconnect_reason = reason; event.diagnostic = text; Post(std::move(event)); }
    catch (...) { out_of_memory_ = true; changed_.notify_all(); }
}
bool SANetClient::NextEvent(SANetEvent& out, uint32_t timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!changed_.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return !events_.empty() || out_of_memory_.load(); }))
        return false;
    if (out_of_memory_.exchange(false)) throw Failure(SD_OUT_OF_MEMORY, "Network event queue allocation failed");
    while (!events_.empty()) {
        queued_bytes_ -= events_.front().packet.payload.size() +
            (events_.front().frame ? events_.front().frame->pixels.size() : 0);
        out = std::move(events_.front()); events_.pop_front();
        if (out.generation == generation_) return true;
    }
    return false;
}
}
