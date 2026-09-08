#include <winsock2.h>
#include <ws2tcpip.h>
#include "frame_decoder.h"
#include "net_client.h"
#include "fstm.h"
#include "pointer_output.h"
#include "renderer_geometry.h"
#include "renderer_d3d11.h"
#include "audio_client.h"
#include "settings.h"
#include "touchpad.h"
#include "usb_client.h"
#include "apk_keyboard.h"
#include "absolute_mouse.h"
#include "client_identity.h"
#include "apk_pen_output.h"
#include "motion_input.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <cmath>

namespace wire = sd::protocol;

namespace {
using sd::Failure;
struct Environment {
    bool com = false;
    Environment() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw Failure(SD_IO_ERROR, "WSAStartup failed");
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            WSACleanup();
            throw Failure(SD_INTERNAL_ERROR, "COM initialization failed");
        }
    }
    ~Environment() { if (com) CoUninitialize(); WSACleanup(); }
};
std::wstring utf16(const char* text) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (!n) throw Failure(SD_INVALID_ARGUMENT, "Device identity must be valid UTF-8");
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), n);
    result.pop_back();
    return result;
}
template<size_t capacity> void put_text(char16_t (&dest)[capacity], const std::wstring& text) {
    if (text.size() >= capacity) throw Failure(SD_INVALID_ARGUMENT, "Device identity is too long for the wire field");
    for (size_t i = 0; i < text.size(); ++i) dest[i] = char16_t(text[i]);
}
bool valid_motion_geometry(const sd_motion_geometry* g) {
    return g && g->struct_size==sizeof(*g) && g->viewport_width && g->viewport_height &&
        g->viewport_width<=INT32_MAX && g->viewport_height<=INT32_MAX &&
        (g->orientation==-1 || (g->orientation>=1 && g->orientation<=4)) && std::isfinite(g->server_aspect_ratio);
}
bool valid_touch_input(const sd_touch_input* input,bool framebuffer_limits=true) {
    if(!input || input->struct_size!=sizeof(*input) || !input->contact_count || input->contact_count>7 ||
       (framebuffer_limits && !sd::valid_size(input->surface_width,input->surface_height))) return false;
    for(uint32_t i=0;i<input->contact_count;++i) if(input->contacts[i].mapped>1) return false;
    return true;
}

}

struct sd_client : private sd::native::SAFstmObserver, private sd::native::SAPointerOutputSink {
    using State = sd::native::SA_FSTM_STATE;
    using Event = sd::native::SA_FSTM_EVENT;
    using Action = sd::native::SAFstmAction;
    DWORD owner = GetCurrentThreadId();
    Environment environment;
    std::shared_ptr<const sd::FrameSnapshot> frame;
    sd_config config{};
    sd::native::SASettings settings;
    sd::native::SASecureSocketLayerConfig tls_config;
    sd::native::SAConnectionWatcherConfig watcher_config;
    char error[256]{};
    sd::native::SANetClient network;
    sd::native::SAUsbClient usb;
    sd_usb_stream usb_stream{};
    bool usb_selected=false,audio_via_usb=false;
    sd::native::SAFstm fstm{*this};
    sd::native::SAMouseOutput mouse_output{*this};
    sd::native::SAPenOutput pen_output{*this};
    sd::native::SATouchOutput touch_output{*this};
    sd::native::APKKeyboardInput android_keyboard;
    sd_mouse_target mouse_target{sizeof(sd_mouse_target),1};
    sd::native::PSRendererGeometry geometry;
    std::shared_ptr<sd::native::PSRendererD3D11> gpu_renderer = std::make_shared<sd::native::PSRendererD3D11>();
    sd::native::SANetAudioClient audio;
    sd_audio_config audio_config{};
    std::wstring audio_endpoint;
    bool audio_requested = false;
    wire::IdentificationPacket identification{};
    std::string address;
    uint16_t port = wire::default_port;
    uint32_t transport = SD_TRANSPORT_TCP;
    bool ready = false, auto_reconnect = false;
    // APK Activity.z1/y1 initialize false and become true on server type4.
    // SDK session boundary replaces the lifetime of a single-host Activity.
    bool stream_control_supported = false;
    uint32_t requested_stream_state = SD_STREAM_UNDEFINED;

    explicit sd_client(const sd_config& c) : config(c) {
        network.SetRenderer(gpu_renderer);
        auto device_id = utf16(c.device_id);
        const auto device_name = utf16(c.device_name);
        if (device_id.empty()) {
            GUID guid{};
            if (FAILED(CoCreateGuid(&guid))) throw Failure(SD_INTERNAL_ERROR, "Cannot generate device ID");
            wchar_t str[40]{};
            if(!StringFromGUID2(guid,str,40)) throw Failure(SD_INTERNAL_ERROR,"Cannot format device ID");
            device_id=str;
        }
        device_id=sd::CanonicalClientId(device_id);
        identification.header = wire::IdentificationHeader(c.width, c.height);
        identification.header.version_minor = c.protocol_minor;
        identification.header.quality = c.quality;
        identification.header.refresh_rate = uint16_t(c.refresh_rate);
        identification.header.license_type = static_cast<wire::LicenseType>(c.license_type);
        put_text(identification.identity.instance_id, device_id);
        put_text(identification.identity.device_name, device_name);
        // Legacy C config keeps its original defaults; its values also enter
        // the unified model. New clients may opt into the APK settings profile.
        settings.Set("client.name",std::string(c.device_name));
        std::string id;
        const int bytes=WideCharToMultiByte(CP_UTF8,0,device_id.data(),int(device_id.size()),nullptr,0,nullptr,nullptr);
        id.resize(size_t(bytes));WideCharToMultiByte(CP_UTF8,0,device_id.data(),int(device_id.size()),id.data(),bytes,nullptr,nullptr);
        settings.Set("client.id",id);
        settings.Set("client.license_type",c.license_type);
        settings.Set("protocol.minor",c.protocol_minor);
        settings.Set("display.quality",c.quality);settings.Set("display.fps",c.refresh_rate);
        settings.Set("display.codec",SD_COMPRESSION_MJPEG);
        settings.Set("display.resolutions",std::to_string(c.width)+"x"+std::to_string(c.height));
        settings.Set("connection.connect_timeout_ms",c.connect_timeout_ms);
        settings.Set("connection.io_timeout_ms",c.io_timeout_ms);
        settings.Set("connection.watcher_poll_ms",0); // Legacy API keeps unlimited idle sessions.
        settings.Set("audio.enabled",0);settings.Set("audio.playback",0);settings.Set("audio.capture_queue_ms",2000);
        sd_audio_config_init(&audio_config);
    }
    ~sd_client() { close(); }
    void close() noexcept {
        retire_pointers();
        ready = false;
        usb.Stop();audio.Stop();
        network.Stop();
        try { fstm.ProcessEvent(Event::SERVER_DISCONNECTED_RECONNECT_OFF); } catch (...) {}
        usb_selected=false;
        stream_control_supported=false;requested_stream_state=SD_STREAM_UNDEFINED;
    }
    bool wait_transmitter(uint32_t timeout) { return usb_selected?usb.WaitForTransmitter(timeout):network.WaitForTransmitter(timeout); }
    void transmit(std::vector<uint8_t> bytes,bool priority=false) {
        if(usb_selected) usb.Transmit(std::move(bytes),priority);else network.Transmit(std::move(bytes),priority);
    }
    template<class Packet> void transmit(const Packet& packet,bool priority=false) {
        const auto bytes=wire::bytes(packet);transmit(std::vector<uint8_t>(bytes.begin(),bytes.end()),priority);
    }
    // Diagnostics keep the audio owner of the last display transport after
    // close() clears its active USB flag.
    sd::native::SANetAudioClient& audio_source() { return audio_via_usb?usb.Audio():audio; }
    sd_result reset_pointers() {
        const auto result = touch_output.Reset(ready);
        mouse_output.Reset(); pen_output.Reset();
        return result;
    }
    void retire_pointers() noexcept {
        const bool flush = ready && touch_output.Active();
        try {
            reset_pointers();
            // Native touch destructor emits final UP. Flush it before priority
            // disconnect can discard queued input, or before the socket closes.
            if (flush) wait_transmitter(config.io_timeout_ms);
        } catch (...) {}
    }
    sd_result Emit(const wire::MouseHeader& packet) override { return send_input(Event::CLIENT_INPUT_MOUSE, packet); }
    sd_result Emit(const wire::PenHeader& packet) override { return send_input(Event::CLIENT_INPUT_PEN, packet); }
    sd_result Emit(const wire::TouchHeader& packet) override { return send_input(Event::CLIENT_INPUT_TOUCH, packet); }
    void Invoke(Action action, void* context) override {
        switch (action) {
        case Action::Connect: case Action::AutoConnect:
        case Action::Reconnect: case Action::ReconnectConnected: case Action::FullscreenReconnect:
            audio.Stop();
            ready = false;
            reset_pointers();
            requested_stream_state=SD_STREAM_UNDEFINED;
            if(usb_selected) usb.Start(usb_stream,identification,audio_requested,audio_config,audio_endpoint,config.io_timeout_ms,gpu_renderer);
            else network.Connect(address, port, config.connect_timeout_ms, config.io_timeout_ms, transport,tls_config,watcher_config);
            frame.reset();
            geometry.FrameBufferSizeSet(0, 0);
            break;
        case Action::ServerConnected:
            if(!usb_selected) network.Start(identification);
            ready = true;
            if (!usb_selected && audio_requested) audio.Start(address,port,identification.identity,audio_config,audio_endpoint,config.connect_timeout_ms,config.io_timeout_ms,tls_config);
            break;
        case Action::Disconnect: case Action::FullscreenDisconnect:
            audio.Stop();
            if (ready) {
                const uint32_t reason = context ? *static_cast<const uint32_t*>(context) : 0;
                if(usb_selected) usb.PrepareDisconnect();else network.PrepareDisconnect();
                transmit(wire::ClientDisconnectHeader{static_cast<wire::ClientDisconnectReason>(reason)}, true);
            }
            ready = false;
            break;
        case Action::InputMouse: case Action::InputKeyboard: case Action::InputTouch: case Action::InputPen:
            if (!ready || !context) throw Failure(SD_IO_ERROR, "Input requires a connected session");
            transmit(*static_cast<std::vector<uint8_t>*>(context));
            break;
        case Action::DisplayOn: case Action::DisposeContext: case Action::LeaveFullscreen:
        case Action::None:
            // Original effects are UI-only (window/fullscreen/text) or disposal
            // of the copied event description. SDK context uses automatic lifetime.
            break;
        }
    }
    void OnEntry(State state, State, Event, void*) override {
        if (state == State::DISCONNECTED) {
            ready = false; usb.Stop();audio.Stop(); reset_pointers(); network.Stop();
            stream_control_supported=false;requested_stream_state=SD_STREAM_UNDEFINED;
        }
    }
    void begin(const char* ip, uint16_t requested_port, bool reconnect) {
        if(settings.Integer("connection.transport")!=0) throw Failure(SD_UNSUPPORTED,"Use start_usb with an actual peripheral stream for the USB profile");
        in_addr parsed{};
        if (!ip || InetPtonA(AF_INET, ip, &parsed) != 1)
            throw Failure(SD_INVALID_ARGUMENT, "An IPv4 literal is required");
        close(); frame.reset();
        audio_via_usb=false;audio.ClearDisconnectState();usb.Audio().ClearDisconnectState();
        address = ip; port = requested_port ? requested_port : wire::default_port;
        auto_reconnect = reconnect;
        fstm.ProcessEvent(reconnect ? Event::MENU_AUTO_CONNECT_SERVER : Event::MENU_CONNECT_SERVER);
    }
    void begin_usb(const sd_usb_stream& stream) {
        close();frame.reset();geometry.FrameBufferSizeSet(0,0);
        audio_via_usb=true;audio.ClearDisconnectState();usb.Audio().ClearDisconnectState();
        usb_stream=stream;usb_selected=true;auto_reconnect=false;
        fstm.ProcessEvent(Event::MENU_CONNECT_SERVER);
    }
    void connect_to(const char* ip, uint16_t requested_port) {
        begin(ip, requested_port, false);
        const auto deadline = GetTickCount64() + config.connect_timeout_ms + config.io_timeout_ms;
        while (!ready) {
            if (GetTickCount64() >= deadline) throw Failure(SD_IO_ERROR, "Connect task timed out");
            sd_event event{};
            const auto status = poll(100, event);
            if (status == SD_DISCONNECTED) throw Failure(SD_IO_ERROR, "Connection task stopped");
        }
        if (!network.WaitForTransmitter(config.io_timeout_ms))
            throw Failure(SD_IO_ERROR, "Identification transmission timed out");
    }
    void disconnect(uint32_t reason) {
        retire_pointers();
        const bool was_ready = ready;
        fstm.ProcessEvent(Event::MENU_DISCONNECT_SERVER, &reason);
        // Synchronous C convenience API waits for the priority disconnect task,
        // then closes and joins workers even if the peer leaves TCP half-open.
        // A pending connect has no disconnect packet to flush. Cancel its
        // socket immediately instead of waiting for the connect timeout.
        if (was_ready && wait_transmitter(config.io_timeout_ms) && !usb_selected)
            network.FinishDisconnect(std::min<uint32_t>(100, config.io_timeout_ms));
        close();
    }
    template<class Packet> void send_packet(const Packet& packet) {
        if (!ready) throw Failure(SD_IO_ERROR, "Client is not connected");
        transmit(packet);
    }
    template<class Packet> sd_result send_input(Event event, const Packet& packet) {
        if ((event==Event::CLIENT_INPUT_MOUSE && !settings.Integer("input.mouse")) ||
            (event==Event::CLIENT_INPUT_KEYBOARD && !settings.Integer("input.keyboard")) ||
            (event==Event::CLIENT_INPUT_TOUCH && !settings.Integer("input.touch")) ||
            (event==Event::CLIENT_INPUT_PEN && !settings.Integer("input.pen"))) return SD_IGNORED;
        const auto bytes = wire::bytes(packet);
        std::vector<uint8_t> data(bytes.begin(), bytes.end());
        return fstm.ProcessEvent(event, &data) ? SD_OK : SD_IGNORED;
    }
    sd_result send_touch_input(const sd_touch_input& input,const sd_motion_geometry* motion_geometry=nullptr) {
        if(!ready || !settings.Integer("input.touch")) return SD_IGNORED;
        sd::native::SAMotionContact contacts[7]{};
        for(uint32_t i=0;i<input.contact_count;++i) {
            const auto& p=input.contacts[i];contacts[i]={p.pointer_id,p.x,p.y,p.mapped!=0};
        }
        const auto event=motion_geometry?sd::native::SAMotionEvent(input.action,*motion_geometry,contacts,input.contact_count):
            sd::native::SAMotionEvent(input.action,contacts,input.contact_count);
        sd::native::APKTouchOutput output(input.surface_width,input.surface_height);
        output.ModeSet(uint32_t(settings.Integer("input.touch_type")));
        const auto packet=output.Build(event,input.timestamp_ms);
        return packet?send_input(Event::CLIENT_INPUT_TOUCH,*packet):SD_IGNORED;
    }
    sd_result connection_lost(sd_result status, const char* diagnostic, sd_event& output) {
        if (diagnostic && *diagnostic) std::snprintf(error, sizeof(error), "%s", diagnostic);
        ready = false;
        if (auto_reconnect && status == SD_IO_ERROR) {
            fstm.ProcessEvent(Event::SERVER_DISCONNECTED_RECONNECT_ON);
            if (fstm.State() != State::DISCONNECTED) {
                output.type = SD_EVENT_CONNECTION_STATE; output.value = uint32_t(fstm.State());
                return SD_OK;
            }
        } else fstm.ProcessEvent(Event::SERVER_DISCONNECTED_RECONNECT_OFF);
        if (status < 0) throw Failure(status, error[0] ? error : "Connection failed");
        output.type = SD_EVENT_DISCONNECTED; return SD_DISCONNECTED;
    }
    sd_result poll(uint32_t timeout, sd_event& output) {
        using Kind = sd::native::SANetEvent::Kind;
        touch_output.ProcessTick(GetTickCount64());
        timeout = std::min(timeout, touch_output.UntilTick(GetTickCount64()));
        sd::native::SANetEvent incoming{Kind::Disconnected};
        if (!(usb_selected?usb.NextEvent(incoming,timeout):network.NextEvent(incoming, timeout))) {
            touch_output.ProcessTick(GetTickCount64());
            if (fstm.State() == State::DISCONNECTED) {
                output.type = SD_EVENT_DISCONNECTED; return SD_DISCONNECTED;
            }
            return SD_TIMEOUT;
        }
        if (incoming.kind == Kind::Connected) {
            fstm.ProcessEvent(Event::SERVER_CONNECTED);
            output.type = SD_EVENT_CONNECTION_STATE; output.value = uint32_t(fstm.State());
            return SD_OK;
        }
        if (incoming.kind == Kind::RendererError) {
            std::snprintf(error, sizeof(error), "%s", incoming.diagnostic.c_str());
            output.type = SD_EVENT_RENDERER_ERROR; output.value = uint32_t(incoming.status);
            return SD_OK;
        }
        if(incoming.kind==Kind::ConnectionWarning) {
            output.type=SD_EVENT_CONNECTION_WARNING;output.value=uint32_t(incoming.status);return SD_OK;
        }
        if (incoming.kind == Kind::Error && incoming.disconnect_reason) {
            output.type=SD_EVENT_CLIENT_DISCONNECTED;output.wire_type=0;output.value=incoming.disconnect_reason;
            disconnect(incoming.disconnect_reason);
            throw Failure(incoming.status, incoming.diagnostic.c_str());
        }
        if (incoming.kind == Kind::Error)
            return connection_lost(incoming.status, incoming.diagnostic.c_str(), output);
        if (incoming.kind == Kind::Disconnected) {
            if (auto_reconnect) return connection_lost(SD_IO_ERROR, "Server disconnected; reconnecting", output);
            return connection_lost(SD_DISCONNECTED, nullptr, output);
        }
        if (incoming.kind == Kind::Frame) {
            frame = std::move(incoming.frame);
            geometry.FrameBufferSizeSet(frame->width, frame->height);
            output.type = SD_EVENT_FRAME; output.wire_type = uint32_t(wire::HeaderType::FRAMEBUFFER);
            output.value = frame->width;
            return SD_OK;
        }
        const auto header = wire::decode<wire::PacketHeader>(incoming.packet.header);
        const auto type = header.prefix.type;
        output.wire_type = uint32_t(type); output.value = header.value8;
        switch (type) {
        case wire::HeaderType::VISIBILITY:
            if (header.value8 > 1) throw Failure(SD_PROTOCOL_ERROR, "Invalid visibility flag");
            fstm.ProcessEvent(header.value8 ? Event::SERVER_DISPLAY_VISIBILITY_ON : Event::SERVER_DISPLAY_VISIBILITY_OFF);
            output.type = SD_EVENT_CONTROL;
            break;
        case wire::HeaderType::PING:
            output.type = SD_EVENT_CONTROL;
            break;
        case wire::HeaderType::DISCONNECT:
            output.value=uint32_t(wire::decode<wire::ServerDisconnectHeader>(incoming.packet.header).reason);
            output.type=SD_EVENT_DISCONNECTED;
            disconnect(SD_DISCONNECT_REGULAR);
            return SD_DISCONNECTED;
        case wire::HeaderType::EVT_DISPLAY_SETTINGS:
            // In the inspected service, type 6 is POWER_S0_EXIT. Preserve that
            // original FSM event; newer settings semantics require a profile.
            fstm.ProcessEvent(Event::SERVER_POWER_SUSPEND);
            output.type = SD_EVENT_CONTROL;
            break;
        case wire::HeaderType::DISPLAY_SETTINGS:
            stream_control_supported=true;
            output.type = SD_EVENT_CONTROL;
            break;
        case wire::HeaderType::UNUSED_01:
            output.type = SD_EVENT_CONTROL;
            break;
        default:
            throw Failure(SD_UNSUPPORTED, "Unsupported server packet type");
        }
        return SD_OK;
    }
};

namespace {
template<class F> sd_result invoke(sd_client* c, F&& f) noexcept {
    if (!c) return SD_INVALID_ARGUMENT;
    if (c->owner != GetCurrentThreadId()) return SD_WRONG_THREAD;
    try { return f(); }
    catch (const sd::native::GpuFailure& e) {
        std::snprintf(c->error, sizeof(c->error), "%s", e.what());
        c->gpu_renderer->MarkFailed(e.hresult); return SD_GPU_ERROR;
    }
    catch (const Failure& e) {
        std::snprintf(c->error, sizeof(c->error), "%s", e.what());
        if (e.status != SD_INVALID_ARGUMENT && e.status != SD_GPU_ERROR) c->close();
        return e.status;
    } catch (const std::bad_alloc&) {
        std::snprintf(c->error, sizeof(c->error), "Out of memory"); c->close(); return SD_OUT_OF_MEMORY;
    } catch (...) {
        std::snprintf(c->error, sizeof(c->error), "Unexpected SDK exception"); c->close(); return SD_INTERNAL_ERROR;
    }
}
}

extern "C" {
const char* SD_CALL sd_client_disconnect_reason_name(uint32_t reason) {
    switch(static_cast<wire::ClientDisconnectReason>(reason)) {
    case wire::ClientDisconnectReason::GRACEFUL_REGULAR:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_CLIENT_GRACEFUL_REGULAR";
    case wire::ClientDisconnectReason::ERROR_DECODING_H264_INIT_FAILED:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_CLIENT_ERROR_DECODING_H264_INIT_FAILED";
    case wire::ClientDisconnectReason::ERROR_DECODING_H264_FAILED:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_CLIENT_ERROR_DECODING_H264_FAILED";
    default:return nullptr;
    }
}
const char* SD_CALL sd_server_disconnect_reason_name(uint32_t reason) {
    switch(static_cast<wire::ServerDisconnectReason>(reason)) {
    case wire::ServerDisconnectReason::GRACEFUL_REGULAR:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_SERVER_GRACEFUL_REGULAR";
    case wire::ServerDisconnectReason::ERROR_NO_MEMORY:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_SERVER_ERROR_NO_MEMORY";
    case wire::ServerDisconnectReason::ERROR_UNSPECIFIED:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_SERVER_ERROR_UNSPECIFIED";
    case wire::ServerDisconnectReason::ERROR_ENCODER_H264_INIT_FAILED:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_SERVER_ERROR_ENCODER_H264_INIT_FAILED";
    case wire::ServerDisconnectReason::ERROR_ENCODING_H264_FAILED:return "PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_SERVER_ERROR_ENCODING_H264_FAILED";
    default:return nullptr;
    }
}
uint32_t SD_CALL sd_abi_version(void) { return 1; }
sd_result SD_CALL sd_viewport_init(sd_viewport* viewport) {
    if (!viewport) return SD_INVALID_ARGUMENT;
    *viewport = {}; viewport->struct_size = sizeof(*viewport);
    viewport->logical_width = 1280; viewport->logical_height = 720;
    viewport->dpi = 96; viewport->composition_scale_x = viewport->composition_scale_y = 1;
    return SD_OK;
}
sd_result SD_CALL sd_pointer_sample_init(sd_pointer_sample* sample) {
    if (!sample) return SD_INVALID_ARGUMENT;
    *sample = {}; sample->struct_size = sizeof(*sample); sample->mapped = 1;
    return SD_OK;
}
sd_result SD_CALL sd_config_init(sd_config* c) {
    if (!c) return SD_INVALID_ARGUMENT;
    *c = {}; c->struct_size = sizeof(*c);
    c->width = 1280; c->height = 720; c->refresh_rate = 30; c->quality = 70;
    c->connect_timeout_ms = 3000; c->io_timeout_ms = 5000; c->protocol_minor = 8;
    std::snprintf(c->device_name, sizeof(c->device_name), "Custom spacedesk SDK client");
    return SD_OK;
}
sd_result SD_CALL sd_display_options_init(sd_display_options* out) {
    if (!out) return SD_INVALID_ARGUMENT;
    *out = {}; out->struct_size = sizeof(*out);
    out->compression = SD_COMPRESSION_MJPEG; out->video_codec = SD_COMPRESSION_MJPEG;
    out->chroma = SD_CHROMA_420; out->quality = 70; out->refresh_rate = 30;
    out->session_type = 4; out->resolution_count = 1; out->resolutions[0] = {1280, 720};
    return SD_OK;
}
sd_result SD_CALL sd_client_create(const sd_config* c, sd_client** out) {
    if (!out) return SD_INVALID_ARGUMENT;
    *out = nullptr;
    if (!c || c->struct_size != sizeof(*c) || !sd::valid_size(c->width, c->height) ||
        c->refresh_rate < 2 || c->refresh_rate > 240 || !c->quality || c->quality > 100 ||
        !c->connect_timeout_ms || c->connect_timeout_ms > 60000 || !c->io_timeout_ms || c->io_timeout_ms > 60000 ||
        (c->protocol_minor != 7 && c->protocol_minor != 8) || c->license_type > 5 ||
        !std::memchr(c->device_id, 0, sizeof(c->device_id)) || !std::memchr(c->device_name, 0, sizeof(c->device_name)))
        return SD_INVALID_ARGUMENT;
    try { *out = new sd_client(*c); return SD_OK; }
    catch (const Failure& e) { return e.status; }
    catch (const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
    catch (...) { return SD_INTERNAL_ERROR; }
}
sd_result SD_CALL sd_client_destroy(sd_client* c) {
    if (!c) return SD_OK;
    if (c->owner != GetCurrentThreadId()) return SD_WRONG_THREAD;
    delete c; return SD_OK;
}
sd_result SD_CALL sd_client_get_settings(sd_client* c,sd_settings* out) {
    if(!out) return SD_INVALID_ARGUMENT;
    if(out->owner!=GetCurrentThreadId()) return SD_WRONG_THREAD;
    return invoke(c,[&]() -> sd_result { auto copy=c->settings;out->value.Swap(copy);return SD_OK; });
}
sd_result SD_CALL sd_client_apply_settings(sd_client* c,const sd_settings* s) {
    if(!s) return SD_INVALID_ARGUMENT;
    if(s->owner!=GetCurrentThreadId()) return SD_WRONG_THREAD;
    return invoke(c,[&]() -> sd_result {
        if(c->fstm.State()!=sd_client::State::DISCONNECTED)
            throw Failure(SD_INVALID_ARGUMENT,"Apply SASettings while disconnected; identification preferences take effect on the next connection");
        try {
            auto next=s->value;
            next.ValidateForClient();
            auto tls=sd::native::SASecureSocketLayerConfig::FromSettings(next);
            const auto mouse_target=next.MouseTarget();
            const sd::native::SAConnectionWatcherConfig watcher{uint32_t(next.Integer("connection.watcher_poll_ms")),uint32_t(next.Integer("connection.watcher_timeout_ms"))};
            if(next.Text("client.id").empty()) next.Set("client.id",c->settings.Text("client.id"));
            auto cfg=c->config;
            const auto modes=next.Resolutions();
            cfg.width=modes[0].width;cfg.height=modes[0].height;
            cfg.quality=uint32_t(std::max<int64_t>(1,next.Integer("display.quality")));
            cfg.refresh_rate=uint32_t(next.Integer("display.fps"));
            cfg.connect_timeout_ms=uint32_t(next.Integer("connection.connect_timeout_ms"));
            cfg.io_timeout_ms=uint32_t(next.Integer("connection.io_timeout_ms"));
            cfg.protocol_minor=uint32_t(next.Integer("protocol.minor"));
            cfg.license_type=uint32_t(next.Integer("client.license_type"));
            std::snprintf(cfg.device_id,sizeof(cfg.device_id),"%s",next.Text("client.id").c_str());
            std::snprintf(cfg.device_name,sizeof(cfg.device_name),"%s",next.Text("client.name").c_str());
            wire::IdentificationPacket packet{};
            auto& h=packet.header;
            h.version_minor=cfg.protocol_minor;h.license_type=wire::LicenseType(cfg.license_type);
            h.client_type=wire::ClientType(next.Integer("client.type"));h.os_type=wire::OsType(next.Integer("client.os"));
            h.compression=wire::Compression(next.Integer("display.compression"));h.video_codec=uint16_t(next.Integer("display.codec"));
            h.chroma=wire::TJSAMP(next.Integer("display.chroma"));h.quality=cfg.quality;h.refresh_rate=uint16_t(cfg.refresh_rate);
            h.session_type=uint16_t(next.Integer("display.session_type"));h.resolution_count=uint32_t(modes.size());
            std::fill(std::begin(h.widths),std::end(h.widths),0u);std::fill(std::begin(h.heights),std::end(h.heights),0u);
            for(size_t i=0;i<modes.size();++i) { h.widths[i]=modes[i].width;h.heights[i]=modes[i].height; }
            put_text(packet.identity.instance_id,utf16(cfg.device_id));put_text(packet.identity.device_name,utf16(cfg.device_name));
            sd_audio_config ac{};sd_audio_config_init(&ac);
            ac.playback=uint32_t(next.Integer("audio.playback"));ac.capture_queue_ms=uint32_t(next.Integer("audio.capture_queue_ms"));
            ac.probe_server=uint32_t(next.Integer("audio.probe_server"));ac.activation_timeout_ms=uint32_t(next.Integer("audio.activation_timeout_ms"));
            std::snprintf(ac.endpoint_id,sizeof(ac.endpoint_id),"%s",next.Text("audio.endpoint_id").c_str());
            auto endpoint=utf16(ac.endpoint_id);
            // Commit only after every allocation/conversion/validation succeeds.
            c->config=cfg;c->identification=packet;c->audio_config=ac;c->audio_endpoint.swap(endpoint);
            c->tls_config=std::move(tls);
            c->watcher_config=watcher;
            c->mouse_target=mouse_target;
            c->transport=next.Integer("connection.transport")==1?SD_TRANSPORT_USB:uint32_t(next.Integer("connection.websocket"));c->audio_requested=next.Integer("audio.enabled")!=0;
            c->settings.Swap(next);c->error[0]=0;
            return SD_OK;
        } catch(const Failure& e) {
            std::snprintf(c->error,sizeof(c->error),"%s",e.what());return e.status;
        }
    });
}
sd_result SD_CALL sd_client_get_tls_state(sd_client* c,sd_tls_state* out) {
    if(!out || out->struct_size!=sizeof(*out)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { if(c->usb_selected) { *out={};out->struct_size=sizeof(*out); } else c->network.TlsStateGet(*out);out->configured=c->tls_config.mode;return SD_OK; });
}
sd_result SD_CALL sd_client_audio_get_tls_state(sd_client* c,sd_tls_state* out) {
    if(!out || out->struct_size!=sizeof(*out)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { c->audio_source().TlsStateGet(*out);out->configured=c->tls_config.mode;return SD_OK; });
}
sd_result SD_CALL sd_client_get_connection_watch_state(sd_client* c,sd_connection_watch_state* out) {
    if(!out || out->struct_size!=sizeof(*out)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { if(c->usb_selected) { *out={};out->struct_size=sizeof(*out); } else { c->network.WatchStateGet(*out);out->enabled=c->watcher_config.period_ms!=0; }return SD_OK; });
}
sd_result SD_CALL sd_client_create_with_settings(const sd_settings* s,sd_client** out) {
    if(!out) return SD_INVALID_ARGUMENT;*out=nullptr;
    if(!s) return SD_INVALID_ARGUMENT;
    if(s->owner!=GetCurrentThreadId()) return SD_WRONG_THREAD;
    sd_config cfg{};sd_config_init(&cfg);sd_client* created=nullptr;
    auto result=sd_client_create(&cfg,&created);
    if(result!=SD_OK) return result;
    result=sd_client_apply_settings(created,s);
    if(result!=SD_OK) { sd_client_destroy(created);return result; }
    *out=created;return SD_OK;
}
sd_result SD_CALL sd_client_get_display_options(sd_client* c, sd_display_options* out) {
    if (!out || out->struct_size != sizeof(*out)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        const auto& h = c->identification.header;
        *out = {}; out->struct_size = sizeof(*out);
        out->compression = uint32_t(h.compression); out->video_codec = h.video_codec;
        out->chroma = uint32_t(h.chroma); out->quality = h.quality;
        out->refresh_rate = h.refresh_rate; out->session_type = h.session_type; out->resolution_count = h.resolution_count;
        for (uint32_t i = 0; i < h.resolution_count; ++i) out->resolutions[i] = {h.widths[i], h.heights[i]};
        return SD_OK;
    });
}
sd_result SD_CALL sd_client_set_transport(sd_client* c, uint32_t transport) {
    if (transport > SD_TRANSPORT_WEBSOCKET) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        if (c->fstm.State() != sd_client::State::DISCONNECTED) return SD_INVALID_ARGUMENT;
        c->settings.Set("connection.websocket",transport);
        c->settings.Set("connection.transport",0);
        c->settings.Set("client.type",transport==SD_TRANSPORT_WEBSOCKET?1:0);
        c->transport = transport;
        c->identification.header.client_type = transport == SD_TRANSPORT_WEBSOCKET
            ? wire::ClientType::DISPLAY_MONITOR_WEB_BROWSER : wire::ClientType::DISPLAY_MONITOR;
        return SD_OK;
    });
}
sd_result SD_CALL sd_client_get_transport(sd_client* c, uint32_t* transport) {
    if (!transport) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { *transport = c->transport; return SD_OK; });
}
sd_result SD_CALL sd_client_set_display_options(sd_client* c, const sd_display_options* options) {
    if (!options || options->struct_size != sizeof(*options) || !options->resolution_count || options->resolution_count > 9 ||
        options->chroma > SD_CHROMA_411 || options->quality > 100 || options->refresh_rate > 65535 || options->session_type > 65535)
        return SD_INVALID_ARGUMENT;
    if ((options->compression != SD_COMPRESSION_OFF && options->compression != SD_COMPRESSION_MJPEG && options->compression != SD_COMPRESSION_H264) ||
        (options->video_codec != SD_COMPRESSION_OFF && options->video_codec != SD_COMPRESSION_MJPEG &&
         options->video_codec != SD_COMPRESSION_H264 && options->video_codec != SD_COMPRESSION_CHOOSE_BEST)) return SD_UNSUPPORTED;
    if ((options->compression != SD_COMPRESSION_OFF || options->video_codec != SD_COMPRESSION_OFF) && !options->quality)
        return SD_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < options->resolution_count; ++i)
        if (!sd::valid_size(options->resolutions[i].width, options->resolutions[i].height)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        if (c->fstm.State() != sd_client::State::DISCONNECTED)
            throw Failure(SD_INVALID_ARGUMENT,"Display options belong to identification; disconnect before changing them and reconnect to send the new preferences");
        auto next=c->settings;
        next.Set("display.compression",options->compression);next.Set("display.codec",options->video_codec);
        next.Set("display.chroma",options->chroma);next.Set("display.quality",options->quality);
        next.Set("display.fps",options->refresh_rate);next.Set("display.session_type",options->session_type);
        std::string resolutions;
        for(uint32_t i=0;i<options->resolution_count;++i) {
            if(i) resolutions+=';';
            resolutions+=std::to_string(options->resolutions[i].width)+"x"+std::to_string(options->resolutions[i].height);
        }
        next.Set("display.resolutions",resolutions);
        auto& h = c->identification.header;
        h.compression = wire::Compression(options->compression); h.video_codec = uint16_t(options->video_codec);
        h.chroma = wire::TJSAMP(options->chroma); h.quality = options->quality;
        h.refresh_rate = uint16_t(options->refresh_rate); h.session_type = uint16_t(options->session_type);
        h.resolution_count = options->resolution_count;
        std::fill(std::begin(h.widths), std::end(h.widths), 0u);
        std::fill(std::begin(h.heights), std::end(h.heights), 0u);
        for (uint32_t i = 0; i < options->resolution_count; ++i) {
            h.widths[i] = options->resolutions[i].width; h.heights[i] = options->resolutions[i].height;
        }
        c->settings.Swap(next);
        return SD_OK;
    });
}
sd_result SD_CALL sd_client_connect(sd_client* c, const char* ip, uint16_t port) {
    return invoke(c, [&]() -> sd_result { c->connect_to(ip, port); return SD_OK; });
}
sd_result SD_CALL sd_client_start(sd_client* c, const char* ip, uint16_t port, uint32_t reconnect) {
    if (reconnect > 1) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { c->begin(ip, port, reconnect != 0); return SD_PENDING; });
}
sd_result SD_CALL sd_client_start_usb(sd_client* c,const sd_usb_stream* stream) {
    if(!stream || stream->struct_size!=sizeof(*stream) || stream->reserved || !stream->read || !stream->write || !stream->cancel)
        return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        if(c->settings.Integer("connection.transport")!=1) return SD_INVALID_ARGUMENT;
        try { c->settings.ValidateForClient(); }
        catch(const Failure& e) { std::snprintf(c->error,sizeof(c->error),"%s",e.what());return e.status; }
        c->begin_usb(*stream);return SD_PENDING;
    });
}
sd_result SD_CALL sd_client_set_stream_state(sd_client* c,uint32_t state) {
    if(state!=1 && state!=2) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        if(!c->ready) return SD_IGNORED;
        if(!c->stream_control_supported) return SD_UNSUPPORTED;
        c->transmit(wire::StreamStateHeader{wire::StreamState(state)});
        c->requested_stream_state=state;return SD_OK;
    });
}
sd_result SD_CALL sd_client_get_stream_control_state(sd_client* c,sd_stream_control_state* state) {
    if(!state || state->struct_size!=sizeof(*state)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        *state={sizeof(*state),uint32_t(c->stream_control_supported),c->requested_stream_state,0};return SD_OK;
    });
}
sd_result SD_CALL sd_client_get_state(sd_client* c, uint32_t* out) {
    if (!out) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { *out = uint32_t(c->fstm.State()); return SD_OK; });
}
sd_result SD_CALL sd_client_set_fullscreen(sd_client* c, uint32_t enabled) {
    if (enabled > 1) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        return c->fstm.ProcessEvent(enabled ? sd_client::Event::CLIENT_FULLSCREEN_ON : sd_client::Event::CLIENT_FULLSCREEN_OFF)
            ? SD_OK : SD_IGNORED;
    });
}
sd_result SD_CALL sd_client_set_auto_reconnect(sd_client* c,uint32_t enabled) {
    if(enabled>1) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { if(c->usb_selected && enabled) return SD_UNSUPPORTED;c->auto_reconnect=enabled!=0;return SD_OK; });
}
sd_result SD_CALL sd_client_disconnect(sd_client* c) {
    return sd_client_disconnect_with_reason(c, SD_DISCONNECT_REGULAR);
}
sd_result SD_CALL sd_client_disconnect_with_reason(sd_client* c, uint32_t reason) {
    if (reason > SD_DISCONNECT_H264_DECODE_FAILED) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        c->disconnect(reason); return SD_OK;
    });
}
sd_result SD_CALL sd_client_poll(sd_client* c, uint32_t timeout, sd_event* out) {
    if (!out || out->struct_size != sizeof(*out) || timeout > 60000) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        *out = {}; out->struct_size = sizeof(*out); return c->poll(timeout, *out);
    });
}
sd_result SD_CALL sd_client_get_frame(sd_client* c, sd_frame* out) {
    if (!out || out->struct_size != sizeof(*out)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        *out = {}; out->struct_size = sizeof(*out);
        if (!c->frame) return SD_NO_FRAME;
        c->frame->View(*out); return SD_OK;
    });
}
sd_result SD_CALL sd_client_get_source_format(sd_client* c, sd_source_format* out) {
    if (!out || out->struct_size != sizeof(*out)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        *out = {}; out->struct_size = sizeof(*out);
        if (!c->frame) return SD_NO_FRAME;
        out->compression = c->frame->compression; out->chroma = c->frame->chroma; out->quality = c->frame->quality;
        out->width = c->frame->width; out->height = c->frame->height; out->sequence = c->frame->sequence;
        return SD_OK;
    });
}
sd_result SD_CALL sd_client_send_mouse(sd_client* c, int32_t x, int32_t y,
    int32_t data, uint32_t buttons, uint32_t flags) {
    return invoke(c, [&]() -> sd_result {
        return c->send_input(sd_client::Event::CLIENT_INPUT_MOUSE, wire::MouseHeader{x, y, data, buttons, flags});
    });
}
sd_result SD_CALL sd_mouse_target_init(sd_mouse_target* target) {
    if(!target) return SD_INVALID_ARGUMENT;*target={};target->struct_size=sizeof(*target);target->rotation=1;return SD_OK;
}
sd_result SD_CALL sd_client_set_mouse_target(sd_client* c,const sd_mouse_target* target) {
    if(!target) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        sd::ValidateMouseTarget(*target);
        auto next=c->settings;
        auto text=[](int32_t x,int32_t y,uint32_t w,uint32_t h) {
            return std::to_string(x)+","+std::to_string(y)+","+std::to_string(w)+","+std::to_string(h);
        };
        next.Set("input.mouse_desktop_rect",target->desktop_width?text(target->desktop_left,target->desktop_top,target->desktop_width,target->desktop_height):std::string{});
        next.Set("input.mouse_target_rect",target->desktop_width?text(target->display_left,target->display_top,target->display_width,target->display_height):std::string{});
        next.Set("input.mouse_target_rotation",target->rotation);
        c->settings.Swap(next);c->mouse_target=*target;return SD_OK;
    });
}
sd_result SD_CALL sd_client_send_mouse_absolute(sd_client* c,float x,float y,uint32_t width,uint32_t height,int32_t data,uint32_t buttons) {
    return invoke(c,[&]() -> sd_result {
        return c->send_input(sd_client::Event::CLIENT_INPUT_MOUSE,sd::AbsoluteMouse(c->mouse_target,x,y,width,height,data,buttons));
    });
}
sd_result SD_CALL sd_client_send_keyboard(sd_client* c, uint32_t vk, uint32_t scan, uint32_t flags) {
    return invoke(c, [&]() -> sd_result {
        return c->send_input(sd_client::Event::CLIENT_INPUT_KEYBOARD, wire::KeyboardHeader{vk, scan, flags});
    });
}
sd_result SD_CALL sd_client_set_input_enabled(sd_client* c,uint32_t type,uint32_t enabled) {
    if(type>SD_INPUT_KEYBOARD || enabled>1) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        constexpr const char* keys[]={"input.touch","input.pen","input.mouse","input.keyboard"};
        if(type==SD_INPUT_TOUCH && !enabled) c->touch_output.Reset(c->ready);
        c->settings.Set(keys[type],enabled);return SD_OK;
    });
}
sd_result SD_CALL sd_client_set_touch_input_type(sd_client* c,uint32_t type) {
    if(type>1) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { c->settings.Set("input.touch_type",type);return SD_OK; });
}
sd_result SD_CALL sd_client_send_android_key(sd_client* c,const sd_android_key_event* event) {
    if(!event || event->struct_size!=sizeof(*event) || event->action>SD_ANDROID_KEY_MULTIPLE ||
       event->caps_lock_on>1 || event->key_code>INT32_MAX) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        if(!c->ready || !c->settings.Integer("input.keyboard")) return SD_IGNORED;
        if(c->identification.header.os_type!=wire::OsType::ANDROID) return SD_UNSUPPORTED;
        const auto packets=c->android_keyboard.Process(*event);
        sd_result result=SD_IGNORED;
        for(uint32_t i=0;i<packets.count;++i) {
            result=c->send_input(sd_client::Event::CLIENT_INPUT_KEYBOARD,packets.values[i]);
            if(result!=SD_OK) return result;
        }
        return result;
    });
}
sd_result SD_CALL sd_client_reset_android_keyboard(sd_client* c) {
    return invoke(c,[&]() -> sd_result { c->android_keyboard.Reset();return SD_OK; });
}
sd_result SD_CALL sd_client_send_unicode(sd_client* c, uint32_t unit) {
    if(unit>0xffff) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        return unit?c->send_input(sd_client::Event::CLIENT_INPUT_KEYBOARD,wire::KeyboardHeader::Unicode(uint16_t(unit))):SD_IGNORED;
    });
}
sd_result SD_CALL sd_client_send_text_utf16(sd_client* c,const uint16_t* text,uint32_t length,uint32_t backspaces) {
    if((length&&!text) || length>4096 || backspaces>4096) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        if(!c->ready || !c->settings.Integer("input.keyboard")) return SD_IGNORED;
        const auto os=c->identification.header.os_type;
        bool keys=backspaces!=0;
        for(uint32_t i=0;i<length;++i) keys=keys||text[i]==' '||text[i]=='\n';
        if(keys && os!=wire::OsType::ANDROID && os!=wire::OsType::WINDOWS_NATIVE && os!=wire::OsType::WINDOWS_UWP)
            return SD_UNSUPPORTED; // No guessed key mapping for other client OS declarations.
        auto send=[&](const wire::KeyboardHeader& packet) {
            return c->send_input(sd_client::Event::CLIENT_INPUT_KEYBOARD,packet);
        };
        auto key=[&](uint32_t android_key,uint32_t windows_key,uint32_t scan) -> sd_result {
            if(os==wire::OsType::ANDROID) return send(wire::KeyboardHeader::AndroidOsk(android_key));
            const auto down=send(wire::KeyboardHeader{windows_key,scan,0});
            return down==SD_OK?send(wire::KeyboardHeader{windows_key,scan,SD_KEY_UP}):down;
        };
        sd_result result=SD_IGNORED;
        for(uint32_t i=0;i<backspaces;++i) {
            result=key(67,8,0x0e);if(result!=SD_OK) return result;
        }
        for(uint32_t i=0;i<length;++i) {
            const auto unit=text[i];if(!unit) continue; // APK D0 filters U+0000.
            result=unit=='\n'?key(66,13,0x1c):unit==' '?key(62,32,0x39):send(wire::KeyboardHeader::Unicode(unit));
            if(result!=SD_OK) return result;
        }
        return result;
    });
}
sd_result SD_CALL sd_client_send_touch(sd_client* c, const sd_touch_event* event) {
    if (!event || event->struct_size != sizeof(*event) || !event->contact_count || event->contact_count > 7 ||
        !sd::valid_size(event->surface_width, event->surface_height)) return SD_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < event->contact_count; ++i)
        for (uint32_t j = 0; j < i; ++j)
            if (event->contacts[i].contact_id == event->contacts[j].contact_id) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        wire::TouchHeader packet{};
        packet.surface_width = event->surface_width; packet.surface_height = event->surface_height;
        packet.timestamp_ms = event->timestamp_ms;
        packet.x = event->contacts[0].x; packet.y = event->contacts[0].y;
        packet.flags = event->contacts[0].flags; packet.contact_id = event->contacts[0].contact_id;
        packet.additional_count = event->contact_count - 1;
        for (uint32_t i = 1; i < event->contact_count; ++i) {
            const auto& input = event->contacts[i];
            packet.additional[i - 1] = {input.x, input.y, input.flags, input.contact_id};
        }
        return c->send_input(sd_client::Event::CLIENT_INPUT_TOUCH, packet);
    });
}
sd_result SD_CALL sd_client_send_touch_input(sd_client* c,const sd_touch_input* input) {
    if(!valid_touch_input(input)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { return c->send_touch_input(*input); });
}
sd_result SD_CALL sd_motion_geometry_init(sd_motion_geometry* g) {
    if(!g) return SD_INVALID_ARGUMENT;
    *g={sizeof(*g),-1,1280,720,-1};return SD_OK;
}
sd_result SD_CALL sd_motion_map_point(const sd_motion_geometry* g,int32_t x,int32_t y,int32_t* mapped_x,int32_t* mapped_y) {
    if(!valid_motion_geometry(g) || !mapped_x || !mapped_y) return SD_INVALID_ARGUMENT;
    if(!sd::native::SAMotionEvent::Map(*g,x,y)) return SD_IGNORED;
    *mapped_x=x;*mapped_y=y;return SD_OK;
}
sd_result SD_CALL sd_client_send_touch_viewport(sd_client* c,const sd_motion_geometry* g,const sd_touch_input* input) {
    if(!valid_motion_geometry(g) || !valid_touch_input(input,false) ||
       input->surface_width!=g->viewport_width || input->surface_height!=g->viewport_height) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { return c->send_touch_input(*input,g); });
}
sd_result SD_CALL sd_client_send_pen(sd_client* c, const sd_pen_event* event) {
    if (!event || event->struct_size != sizeof(*event) || !sd::valid_size(event->surface_width, event->surface_height) ||
        !std::isfinite(event->x) || !std::isfinite(event->y) || !std::isfinite(event->pressure) ||
        event->pressure < 0 || event->pressure > 1 || event->contact > 1 || event->in_range > 1)
        return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        wire::PenHeader packet{};
        packet.surface_width = event->surface_width; packet.surface_height = event->surface_height;
        packet.x = event->x; packet.y = event->y; packet.pressure = event->pressure;
        packet.contact = uint8_t(event->contact); packet.in_range = uint8_t(event->in_range);
        return c->send_input(sd_client::Event::CLIENT_INPUT_PEN, packet);
    });
}
sd_result SD_CALL sd_client_send_pen_viewport(sd_client* c,const sd_motion_geometry* geometry,const sd_motion_event* input,uint32_t kind) {
    if(!valid_motion_geometry(geometry) || !sd::ValidMotionEvent(input,false) || kind>SD_PEN_MOTION_HOVER) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        if(!c->ready || !c->settings.Integer("input.pen")) return SD_IGNORED;
        const sd::native::SAMotionEvent event(*geometry,*input);
        if(!event.Valid()) return SD_IGNORED;
        const sd::native::APKPenOutput output(geometry->viewport_width,geometry->viewport_height);
        const auto packet=kind==SD_PEN_MOTION_HOVER?output.Hover(event):output.Contact(event);
        return c->send_input(sd_client::Event::CLIENT_INPUT_PEN,packet);
    });
}
sd_result SD_CALL sd_client_send_pen_button(sd_client* c,uint32_t released) {
    if(released>1) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        const sd::native::APKPenOutput output(0,0);
        return c->send_input(sd_client::Event::CLIENT_INPUT_KEYBOARD,output.Button(released!=0));
    });
}
sd_result SD_CALL sd_client_send_mapped_motion(sd_client* c,uint32_t output,const sd_motion_surface* surface,const sd_motion_event* input) {
    if(output>SD_MOTION_OUTPUT_PEN_HOVER || !surface || surface->struct_size!=sizeof(*surface) ||
       !surface->width || !surface->height || surface->width>INT32_MAX || surface->height>INT32_MAX ||
       !sd::ValidMotionEvent(input,false) || (surface->mapped_mask>>input->pointer_count)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        if(!c->ready) return SD_IGNORED;
        const sd::native::SAMotionEvent event(*input,surface->mapped_mask);if(!event.Valid()) return SD_IGNORED;
        if(output==SD_MOTION_OUTPUT_TOUCH) {
            sd::native::APKTouchOutput builder(surface->width,surface->height);
            builder.ModeSet(uint32_t(c->settings.Integer("input.touch_type")));
            const auto packet=builder.Build(event,surface->timestamp_ms);
            return packet?c->send_input(sd_client::Event::CLIENT_INPUT_TOUCH,*packet):SD_IGNORED;
        }
        const sd::native::APKPenOutput builder(surface->width,surface->height);
        const auto packet=output==SD_MOTION_OUTPUT_PEN_HOVER?builder.Hover(event):builder.Contact(event);
        return c->send_input(sd_client::Event::CLIENT_INPUT_PEN,packet);
    });
}
sd_result SD_CALL sd_client_send_pointer(sd_client* c, uint32_t output, uint32_t event, const sd_pointer_sample* s) {
    if (!s || s->struct_size != sizeof(*s) || output > SD_POINTER_MOUSE || event > SD_POINTER_RAW_MOUSE_MOVE ||
        s->device_type > SD_POINTER_MOUSE || s->update_kind > 10 || s->buttons > 7 ||
        s->mapped > 1 || s->capture_succeeded > 1 || s->in_range > 1 ||
        !std::isfinite(s->x) || !std::isfinite(s->y) || !std::isfinite(s->pressure) ||
        !std::isfinite(s->x_tilt) || !std::isfinite(s->y_tilt) || s->pressure < 0 || s->pressure > 1)
        return SD_INVALID_ARGUMENT;
    if (output != SD_POINTER_MOUSE && s->mapped && !sd::valid_size(s->surface_width, s->surface_height))
        return SD_INVALID_ARGUMENT;
    if (output == SD_POINTER_TOUCH && (s->pointer_id > 65535 || double(s->x) < -2147483648.0 ||
        double(s->x) >= 2147483648.0 || double(s->y) < -2147483648.0 || double(s->y) >= 2147483648.0))
        return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        if (!c->ready) return SD_IGNORED;
        if ((output==SD_POINTER_MOUSE && !c->settings.Integer("input.mouse")) ||
            (output==SD_POINTER_TOUCH && !c->settings.Integer("input.touch")) ||
            (output==SD_POINTER_PEN && !c->settings.Integer("input.pen"))) return SD_IGNORED;
        if (event == SD_POINTER_RAW_MOUSE_MOVE)
            return output == SD_POINTER_MOUSE ? c->mouse_output.OnRawMouseMoved(*s) : SD_IGNORED;
        sd::native::SAPointerOutput* handler = output == SD_POINTER_TOUCH
            ? static_cast<sd::native::SAPointerOutput*>(&c->touch_output) : output == SD_POINTER_PEN
            ? static_cast<sd::native::SAPointerOutput*>(&c->pen_output) : static_cast<sd::native::SAPointerOutput*>(&c->mouse_output);
        const auto now = GetTickCount64();
        switch (event) {
        case SD_POINTER_MOVED: return handler->OnPointerMoved(*s, now);
        case SD_POINTER_PRESSED: return handler->OnPointerPressed(*s, now);
        case SD_POINTER_RELEASED: return handler->OnPointerReleased(*s, now);
        case SD_POINTER_CAPTURE_LOST: return handler->OnPointerCaptureLost(*s, now);
        case SD_POINTER_WHEEL: return handler->OnPointerWheelChanged(*s, now);
        default: return SD_IGNORED;
        }
    });
}
sd_result SD_CALL sd_client_set_touch_mode(sd_client* c, uint32_t mode) {
    if (mode > 1) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { return c->touch_output.SetMode(mode != 0); });
}
sd_result SD_CALL sd_client_reset_pointer_output(sd_client* c) {
    return invoke(c, [&]() -> sd_result { return c->reset_pointers(); });
}
sd_result SD_CALL sd_client_set_rotation(sd_client* c, uint32_t rotation) {
    return invoke(c, [&]() -> sd_result {
        c->send_packet(wire::RotationHeader{rotation}); return SD_OK;
    });
}
sd_result SD_CALL sd_client_set_viewport(sd_client* c, const sd_viewport* v) {
    if (!v || v->struct_size != sizeof(*v) || !std::isfinite(v->logical_width) || !std::isfinite(v->logical_height) ||
        !std::isfinite(v->dpi) || !std::isfinite(v->composition_scale_x) || !std::isfinite(v->composition_scale_y) ||
        v->logical_width < 0 || v->logical_height < 0 || v->dpi <= 0 ||
        v->composition_scale_x <= 0 || v->composition_scale_y <= 0 ||
        !std::isfinite(v->dpi * v->logical_width) || !std::isfinite(v->dpi * v->logical_height)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { c->geometry.WindowSizeSet(*v); c->gpu_renderer->ViewportSet(*v); return SD_OK; });
}
sd_result SD_CALL sd_client_get_view_state(sd_client* c, sd_view_state* state) {
    if (!state || state->struct_size != sizeof(*state)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        if (c->gpu_renderer->Enabled()) c->gpu_renderer->ViewStateGet(*state); else c->geometry.StateGet(*state);
        return SD_OK;
    });
}
sd_result SD_CALL sd_client_view_zoom(sd_client* c, float delta) {
    if (!std::isfinite(delta)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { c->geometry.Zoom(delta); c->gpu_renderer->Zoom(delta); return SD_OK; });
}
sd_result SD_CALL sd_client_view_pan(sd_client* c, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { c->geometry.Pan(x, y); c->gpu_renderer->Pan(x,y); return SD_OK; });
}
sd_result SD_CALL sd_client_view_rotate(sd_client* c, float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result { c->geometry.Rotate(x, y, z); c->gpu_renderer->Rotate(x,y,z); return SD_OK; });
}
sd_result SD_CALL sd_client_view_reset(sd_client* c) {
    return invoke(c, [&]() -> sd_result { c->geometry.Reset(); c->gpu_renderer->TransformReset(); return SD_OK; });
}
sd_result SD_CALL sd_client_view_set_orientation(sd_client* c,uint32_t orientation) {
    if(orientation<1 || orientation>4) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        c->geometry.OrientationSet(orientation);c->gpu_renderer->OrientationSet(orientation);return SD_OK;
    });
}
sd_result SD_CALL sd_client_view_get_orientation(sd_client* c,uint32_t* orientation) {
    if(!orientation) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { *orientation=c->geometry.OrientationGet();return SD_OK; });
}
sd_result SD_CALL sd_client_map_pointer_sample(sd_client* c, sd_pointer_sample* s) {
    if (!s || s->struct_size != sizeof(*s) || !std::isfinite(s->x) || !std::isfinite(s->y)) return SD_INVALID_ARGUMENT;
    return invoke(c, [&]() -> sd_result {
        if (c->gpu_renderer->Enabled())
            return c->gpu_renderer->InputTransform(s->x,s->y,*s) ? SD_OK : SD_NO_FRAME;
        s->surface_width = c->geometry.Width(); s->surface_height = c->geometry.Height(); s->mapped = 0;
        if (!c->geometry.HasFrame()) return SD_NO_FRAME;
        DirectX::XMFLOAT2 point{};
        if (c->geometry.InputTransform(s->x, s->y, point)) { s->x = point.x; s->y = point.y; s->mapped = 1; }
        return SD_OK;
    });
}
sd_result SD_CALL sd_renderer_config_init(sd_renderer_config* config) {
    if (!config) return SD_INVALID_ARGUMENT;
    *config = {}; config->struct_size = sizeof(*config); config->present_interval = 1; return SD_OK;
}
sd_result SD_CALL sd_client_renderer_attach(sd_client* c,const sd_renderer_config* config) {
    if (!config || config->struct_size != sizeof(*config) || config->target > SD_RENDERER_COMPOSITION ||
        config->driver > SD_RENDERER_WARP || config->present_interval > 4 ||
        (config->target != SD_RENDERER_OFFSCREEN && !config->target_object) ||
        (config->target == SD_RENDERER_OFFSCREEN && config->target_object)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { c->gpu_renderer->Initialize(*config,c->geometry); return SD_OK; });
}
sd_result SD_CALL sd_client_renderer_detach(sd_client* c) {
    return invoke(c,[&]() -> sd_result { c->gpu_renderer->Detach(); return SD_OK; });
}
sd_result SD_CALL sd_client_renderer_get_state(sd_client* c,sd_renderer_state* state) {
    if (!state || state->struct_size != sizeof(*state)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { c->gpu_renderer->StateGet(*state); return SD_OK; });
}
sd_result SD_CALL sd_client_renderer_present(sd_client* c) {
    return invoke(c,[&]() -> sd_result { c->gpu_renderer->Present(false); return SD_OK; });
}
sd_result SD_CALL sd_client_renderer_read_pixels(sd_client* c,uint8_t* buffer,uint64_t capacity,sd_frame* frame) {
    if (!frame || frame->struct_size != sizeof(*frame)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { return c->gpu_renderer->ReadPixels(buffer,capacity,*frame); });
}
sd_result SD_CALL sd_client_renderer_copy_texture(sd_client* c,sd_gpu_texture** texture) {
    if (!texture) return SD_INVALID_ARGUMENT; *texture = nullptr;
    return invoke(c,[&]() -> sd_result {
        auto result = c->gpu_renderer->CopyTexture(); if (!result) return SD_NO_FRAME;
        *texture = result.release(); return SD_OK;
    });
}
sd_result SD_CALL sd_gpu_texture_get_native(sd_gpu_texture* texture,void** native,void** shared) {
    if (!texture || (!native && !shared)) return SD_INVALID_ARGUMENT;
    if (native) *native = texture->texture.Get(); if (shared) *shared = texture->shared; return SD_OK;
}
sd_result SD_CALL sd_gpu_texture_read_pixels(sd_gpu_texture* texture,uint8_t* buffer,uint64_t capacity,sd_frame* frame) {
    if (!texture || !frame || frame->struct_size != sizeof(*frame)) return SD_INVALID_ARGUMENT;
    try { return texture->ReadPixels(buffer,capacity,*frame); }
    catch (const sd::native::GpuFailure&) { return SD_GPU_ERROR; }
    catch (const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
    catch (...) { return SD_INTERNAL_ERROR; }
}
sd_result SD_CALL sd_gpu_texture_destroy(sd_gpu_texture* texture) { delete texture; return SD_OK; }
sd_result SD_CALL sd_audio_config_init(sd_audio_config* config) {
    if (!config) return SD_INVALID_ARGUMENT;
    *config={};config->struct_size=sizeof(*config);config->capture_queue_ms=2000;config->probe_server=1;config->activation_timeout_ms=3000;
    return SD_OK;
}
sd_result SD_CALL sd_client_audio_start(sd_client* c,const sd_audio_config* config) {
    if (!config || config->struct_size!=sizeof(*config) || config->playback>1 || config->probe_server>1 ||
        (!config->playback && !config->capture_queue_ms) || (config->capture_queue_ms && (config->capture_queue_ms<10 || config->capture_queue_ms>10000)) ||
        !config->activation_timeout_ms || config->activation_timeout_ms>60000 || !std::memchr(config->endpoint_id,0,sizeof(config->endpoint_id))) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        if(c->usb_selected) return SD_UNSUPPORTED;
        if (!c->ready) return SD_IGNORED;
        const auto endpoint=utf16(config->endpoint_id);
        auto next=c->settings;
        next.Set("audio.enabled",1);next.Set("audio.playback",config->playback);next.Set("audio.capture_queue_ms",config->capture_queue_ms);
        next.Set("audio.probe_server",config->probe_server);next.Set("audio.activation_timeout_ms",config->activation_timeout_ms);
        next.Set("audio.endpoint_id",std::string(config->endpoint_id));
        const auto result=c->audio.Start(c->address,c->port,c->identification.identity,*config,endpoint,c->config.connect_timeout_ms,c->config.io_timeout_ms,c->tls_config);
        if (result==SD_PENDING) { c->audio_requested=true;c->audio_config=*config;c->audio_endpoint=endpoint;c->settings.Swap(next); }
        else std::snprintf(c->error,sizeof(c->error),"%s",c->audio.LastError().c_str());
        return result;
    });
}
sd_result SD_CALL sd_client_audio_stop(sd_client* c) {
    return invoke(c,[&]() -> sd_result { if(c->usb_selected) return SD_UNSUPPORTED;c->audio_requested=false;c->settings.Set("audio.enabled",0);c->audio.Stop();return SD_OK; });
}
sd_result SD_CALL sd_client_audio_get_state(sd_client* c,sd_audio_state* state) {
    if (!state || state->struct_size!=sizeof(*state)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        c->audio_source().StateGet(*state);
        if (state->state==SD_AUDIO_FAILED) std::snprintf(c->error,sizeof(c->error),"%s",c->audio_source().LastError().c_str());
        return SD_OK;
    });
}
sd_result SD_CALL sd_client_audio_get_disconnect_state(sd_client* c,sd_audio_disconnect_state* state) {
    if(!state || state->struct_size!=sizeof(*state)) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result { c->audio_source().DisconnectStateGet(*state);return SD_OK; });
}
sd_result SD_CALL sd_client_audio_poll(sd_client* c,uint32_t timeout,sd_audio_buffer* buffer) {
    if (!buffer || buffer->struct_size!=sizeof(*buffer) || timeout>60000) return SD_INVALID_ARGUMENT;
    return invoke(c,[&]() -> sd_result {
        *buffer={};buffer->struct_size=sizeof(*buffer);
        const auto result=c->audio_source().Poll(timeout,*buffer);
        if (result<0) std::snprintf(c->error,sizeof(c->error),"%s",c->audio_source().LastError().c_str());
        return result;
    });
}
const char* SD_CALL sd_client_last_error(const sd_client* c) {
    if (!c) return "Invalid client handle";
    if (c->owner != GetCurrentThreadId()) return "Wrong SDK owner thread";
    return c->error;
}
}
