#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Native little-endian wire structures for Windows x64. No vptrs or pointers.
// Enum type names follow preserved APK constant prefixes. Packet struct names
// are descriptive aliases: their exact original C++ type spelling is unknown.
// Runtime class inheritance belongs outside these serialized data structures.
namespace sd::protocol {
enum class PROTOCOL_SPCDSK_SMPL_HEADER_TYPE : uint32_t {
    IDENTIFICATION = 0, PING = 1, FRAMEBUFFER = 2, VISIBILITY = 3,
    DISPLAY_SETTINGS = 4, UNUSED_01 = 5, EVT_DISPLAY_SETTINGS = 6,
    FLOW_CONTROL_ACK = 7, DISCONNECT = 8, ROTATION = 9,
    EVT_INPUT_MOUSE = 10, EVT_INPUT_KEYBOARD = 11, EVT_INPUT_TOUCH = 12,
    EVT_INPUT_PEN = 13, AUDIO = 14, MAX = 15
};
enum class PROTOCOL_SPCDSK_SMPL_CLIENT_TYPE : uint32_t {
    DISPLAY_MONITOR = 0, DISPLAY_MONITOR_WEB_BROWSER = 1, RESERVED_01 = 2, AUDIO = 3, MAX = 4
};
enum class PROTOCOL_SPCDSK_SMPL_COMPRESSION : uint32_t {
    OFF = 0, YUV_PLAIN = 1, UNUSED_01 = 2, MJPEG = 3, H264 = 4, CHOOSE_BEST = 5, MAX = 6
};
enum class PROTOCOL_SPCDSK_SMPL_OS_TYPE : uint32_t {
    UNKNOWN = 0, WINDOWS_NATIVE = 1, WINDOWS_UWP = 2, ANDROID = 3, IOS = 4, MAX = 5
};
enum class PROTOCOL_SPCDSK_LICENSE_TYPE : uint32_t {
    NONE = 0, NONCOMMERCIAL_BASIC = 1, NONCOMMERCIAL_ADVANCED = 2,
    BUSINESS_ANDROID = 3, BUSINESS_WINDOWS_UWP = 4, BUSINESS_IOS = 5, MAX = 6
};
enum class SPCDSK_NET_PROTOCOL_SMPL_PING_OPTION : uint32_t {
    LEGACY = 0, ECHO_REQUEST = 1, ECHO_REPLY = 2, MAX = 3
};
enum class PROTOCOL_SPCDSK_STREAM_STATE : uint32_t { UNDEFINED = 0, PAUSE = 1, PLAY = 2, MAX = 3 };
enum class PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_CLIENT : uint32_t {
    GRACEFUL_REGULAR=0,ERROR_DECODING_H264_INIT_FAILED=1,ERROR_DECODING_H264_FAILED=2,MAX=3
};
enum class PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_SERVER : uint32_t {
    GRACEFUL_REGULAR=0,ERROR_NO_MEMORY=1,ERROR_UNSPECIFIED=2,
    ERROR_ENCODER_H264_INIT_FAILED=3,ERROR_ENCODING_H264_FAILED=4,MAX=5
};
using ClientDisconnectReason=PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_CLIENT;
using ServerDisconnectReason=PROTOCOL_SPCDSK_SMPL_DISCONNECT_REASON_SERVER;
enum class TJSAMP : uint32_t { YUV444 = 0, YUV422 = 1, YUV420 = 2, GRAY = 3, YUV440 = 4, YUV411 = 5 };
using HeaderType = PROTOCOL_SPCDSK_SMPL_HEADER_TYPE;
using ClientType = PROTOCOL_SPCDSK_SMPL_CLIENT_TYPE;
using Compression = PROTOCOL_SPCDSK_SMPL_COMPRESSION;
using OsType = PROTOCOL_SPCDSK_SMPL_OS_TYPE;
using LicenseType = PROTOCOL_SPCDSK_LICENSE_TYPE;
using PingOption = SPCDSK_NET_PROTOCOL_SMPL_PING_OPTION;
using StreamState = PROTOCOL_SPCDSK_STREAM_STATE;
// Descriptive SDK type name; UWP AudioConnect tests discovery +280 bit 0x08.
enum class ServerFeature : uint32_t { AUDIO = 0x08, ENCRYPTION = 0x10 };

constexpr uint16_t default_port = 28252;
constexpr size_t header_size = 128;
constexpr size_t identification_data_size = 334;
constexpr size_t max_resolutions = 9;
constexpr size_t max_additional_contacts = 6;

#pragma pack(push, 1)
struct PacketPrefix {
    HeaderType type;
    uint32_t payload_length = 0;
    explicit PacketPrefix(HeaderType kind = HeaderType::PING, uint32_t count = 0)
        : type(kind), payload_length(count) {}
};
struct PacketHeader {
    PacketPrefix prefix{};
    uint32_t value8 = 0;
    uint32_t value12 = 0;
    uint8_t reserved16[112]{};
};
template<HeaderType Kind> struct EmptyHeader {
    PacketPrefix prefix{Kind};
    uint8_t reserved8[120]{};
};
using FlowControlAckHeader = EmptyHeader<HeaderType::FLOW_CONTROL_ACK>;
// APK x2 -> q2.o -> Activity.u2: this control enables PAUSE/PLAY. No geometry
// fields are read. Descriptive role alias, not a recovered original struct name.
using StreamControlAnnouncementHeader = EmptyHeader<HeaderType::DISPLAY_SETTINGS>;
// In the inspected older service, server->client type 6 means POWER_S0_EXIT.
using LegacyPowerS0ExitHeader = EmptyHeader<HeaderType::EVT_DISPLAY_SETTINGS>;

struct IdentificationHeader {
    PacketPrefix prefix{HeaderType::IDENTIFICATION, identification_data_size};
    uint32_t version_major = 4, version_minor = 8;
    ClientType client_type = ClientType::DISPLAY_MONITOR;
    OsType os_type = OsType::WINDOWS_NATIVE;
    Compression compression = Compression::MJPEG;
    TJSAMP chroma = TJSAMP::YUV420;
    uint32_t quality = 70;
    uint16_t video_codec = uint16_t(Compression::MJPEG);
    uint16_t reserved38 = 1; // Both APK/UWP constructors set 1; semantic name unresolved.
    uint32_t reserved40 = 0;
    uint16_t refresh_rate = 30;
    uint16_t session_type = 4; // APK USB uses 4 without audio, 6 with multiplexed audio.
    uint32_t resolution_count = 1;
    uint32_t widths[max_resolutions]{};
    uint32_t heights[max_resolutions]{};
    LicenseType license_type = LicenseType::NONE;
    IdentificationHeader(uint32_t width = 1280, uint32_t height = 720) {
        widths[0] = width; heights[0] = height;
    }
};
struct IdentificationData {
    char16_t instance_id[39]{};
    char16_t device_name[128]{};
};
struct IdentificationPacket {
    IdentificationHeader header{};
    IdentificationData identity{};
};
// USB sends this header without payload immediately before IdentificationPacket.
// It has the same fields, but a zero following-data length; never an extra envelope.
inline IdentificationHeader UsbInitialIdentification(const IdentificationHeader& display) {
    auto initial=display;initial.prefix.payload_length=0;return initial;
}
struct AudioIdentificationHeader {
    PacketPrefix prefix{HeaderType::IDENTIFICATION, identification_data_size};
    uint32_t version_major = 4, version_minor = 8;
    ClientType client_type = ClientType::AUDIO;
    OsType os_type = OsType::WINDOWS_UWP;
    uint8_t reserved24[104]{};
};
struct AudioIdentificationPacket {
    AudioIdentificationHeader header{};
    IdentificationData identity{};
};
struct AudioHeader {
    PacketPrefix prefix{HeaderType::AUDIO};
    uint8_t reserved8[120]{};
    explicit AudioHeader(uint32_t bytes = 0) { prefix.payload_length = bytes; }
};
struct Rectangle {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
};
struct FramebufferHeader {
    PacketPrefix prefix{HeaderType::FRAMEBUFFER};
    uint32_t width = 0, height = 0;
    int32_t pitch = 0;
    uint32_t pixel_format = 21;
    Rectangle update{};
    Compression compression = Compression::MJPEG;
    TJSAMP chroma = TJSAMP::YUV420;
    uint32_t quality = 0;
    uint8_t reserved52[8]{};
    int16_t fragment_count = 0; // UWP +60: explicit count; 0 selects legacy 1/4.
    uint16_t reserved62 = 0;
    uint32_t four_fragments = 0;
    uint32_t fragment_payload_length = 0;
    uint8_t reserved72[56]{};
};
struct PingHeader {
    PacketPrefix prefix{HeaderType::PING};
    uint32_t reserved8 = 0;
    PingOption option;
    uint8_t reserved16[112]{};
    explicit PingHeader(PingOption value = PingOption::LEGACY) : option(value) {}
};
struct VisibilityHeader {
    PacketPrefix prefix{HeaderType::VISIBILITY};
    uint32_t visible = 0;
    uint8_t reserved12[116]{};
};
struct DisconnectHeader {
    PacketPrefix prefix{HeaderType::DISCONNECT};
    uint32_t reason;
    uint8_t reserved12[116]{};
    explicit DisconnectHeader(uint32_t value = 0) : reason(value) {}
};
// Direction-specific reason domains from APK b.i/b.j. Generic DisconnectHeader
// remains available for existing C++ consumers using a raw uint32 reason.
template<class Reason> struct DisconnectReasonHeader {
    PacketPrefix prefix{HeaderType::DISCONNECT};
    Reason reason=Reason::GRACEFUL_REGULAR;
    uint8_t reserved12[116]{};
    explicit DisconnectReasonHeader(Reason value=Reason::GRACEFUL_REGULAR):reason(value) {}
};
using ClientDisconnectHeader=DisconnectReasonHeader<ClientDisconnectReason>;
using ServerDisconnectHeader=DisconnectReasonHeader<ServerDisconnectReason>;
struct RotationHeader {
    PacketPrefix prefix{HeaderType::ROTATION};
    uint32_t rotation;
    uint8_t reserved12[116]{};
    explicit RotationHeader(uint32_t value = 0) : rotation(value) {}
};
struct StreamStateHeader {
    PacketPrefix prefix{HeaderType::EVT_DISPLAY_SETTINGS};
    StreamState state;
    uint8_t reserved12[116]{};
    explicit StreamStateHeader(StreamState value = StreamState::UNDEFINED) : state(value) {}
};
struct MouseHeader {
    PacketPrefix prefix{HeaderType::EVT_INPUT_MOUSE};
    int32_t x, y, button_data;
    uint32_t button_flags, flags;
    uint8_t reserved28[100]{};
    MouseHeader(int32_t px = 0, int32_t py = 0, int32_t data = 0, uint32_t buttons = 0, uint32_t f = 0)
        : x(px), y(py), button_data(data), button_flags(buttons), flags(f) {}
};
struct KeyboardHeader {
    PacketPrefix prefix{HeaderType::EVT_INPUT_KEYBOARD};
    uint32_t virtual_key;
    union { uint32_t scan_code; uint32_t unicode_utf16; };
    uint32_t flags;
    uint8_t reserved20[108]{};
    KeyboardHeader(uint32_t key = 0, uint32_t scan = 0, uint32_t f = 0)
        : virtual_key(key), scan_code(scan), flags(f) {}
    // S1.b: source name unknown; flag4 selects the service's Unicode sink.
    static KeyboardHeader Unicode(uint16_t unit) { return {0, unit, 4}; }
    // S1.a / SAActivityDisplay.KeyEventOSKSend; key uses Android's namespace.
    static KeyboardHeader AndroidOsk(uint32_t key) { return {key, 0, 0x10000}; }
};
// APK W1.d / C0686e.f0: each extra record stores X, Y, flags, contact ID.
struct TouchContact {
    int32_t x = 0, y = 0;
    uint16_t flags = 0, contact_id = 0;
};
struct TouchHeader {
    PacketPrefix prefix{HeaderType::EVT_INPUT_TOUCH};
    int32_t x = 0, y = 0;
    uint32_t surface_width = 0, surface_height = 0;
    uint16_t flags = 0, contact_id = 0;
    uint32_t timestamp_ms = 0;
    uint32_t additional_count = 0;
    TouchContact additional[max_additional_contacts]{};
    uint8_t reserved108[20]{};
};
// APK V1 + C0686e and UWP SAFstmEvent_State030406_OnClientInputPen.
// UWP reads tilt in its UI layer but zeros reserved24/37 in the wire packet.
struct PenHeader {
    PacketPrefix prefix{HeaderType::EVT_INPUT_PEN};
    uint32_t surface_width = 0, surface_height = 0;
    float x = 0, y = 0;
    uint8_t reserved24[8]{};
    float pressure = 0;
    uint8_t contact = 0;
    uint8_t reserved37[2]{};
    uint8_t in_range = 0;
    uint8_t reserved40[88]{};
};
struct DiscoveryQuery {
    char signature[21] = "SPACEDESK-NET-CLIENT";
    uint8_t reserved21[1003]{};
};
struct LegacyDiscoveryReply {
    char16_t hostname[128]{};
    uint32_t reserved256 = 0;
    uint16_t tcp_port = default_port, reserved262 = 0;
};
struct DiscoveryReply {
    LegacyDiscoveryReply legacy{};
    uint32_t server_os = 0, reserved268 = 0;
    uint32_t version_major = 0, version_minor = 0;
    union { uint32_t feature_flags = 3; Compression compression; }; // Old compression name is an ABI/source alias.
    uint8_t reserved284[24]{};
};
#pragma pack(pop)

#define SD_WIRE_ASSERT(Type, Size) \
    static_assert(sizeof(Type) == Size); \
    static_assert(std::is_standard_layout_v<Type> && std::is_trivially_copyable_v<Type>)
SD_WIRE_ASSERT(PacketPrefix, 8);
SD_WIRE_ASSERT(PacketHeader, 128);
SD_WIRE_ASSERT(IdentificationHeader, 128);
SD_WIRE_ASSERT(IdentificationData, 334);
SD_WIRE_ASSERT(IdentificationPacket, 462);
SD_WIRE_ASSERT(AudioIdentificationHeader, 128);
SD_WIRE_ASSERT(AudioIdentificationPacket, 462);
SD_WIRE_ASSERT(AudioHeader, 128);
SD_WIRE_ASSERT(FramebufferHeader, 128);
SD_WIRE_ASSERT(FlowControlAckHeader, 128);
SD_WIRE_ASSERT(StreamControlAnnouncementHeader, 128);
SD_WIRE_ASSERT(LegacyPowerS0ExitHeader, 128);
SD_WIRE_ASSERT(PingHeader, 128);
SD_WIRE_ASSERT(VisibilityHeader, 128);
SD_WIRE_ASSERT(DisconnectHeader, 128);
SD_WIRE_ASSERT(ClientDisconnectHeader, 128);
SD_WIRE_ASSERT(ServerDisconnectHeader, 128);
static_assert(offsetof(ClientDisconnectHeader,reason)==8 && offsetof(ServerDisconnectHeader,reason)==8);
SD_WIRE_ASSERT(RotationHeader, 128);
SD_WIRE_ASSERT(StreamStateHeader, 128);
SD_WIRE_ASSERT(MouseHeader, 128);
SD_WIRE_ASSERT(KeyboardHeader, 128);
SD_WIRE_ASSERT(TouchHeader, 128);
SD_WIRE_ASSERT(TouchContact, 12);
SD_WIRE_ASSERT(PenHeader, 128);
SD_WIRE_ASSERT(DiscoveryQuery, 1024);
SD_WIRE_ASSERT(LegacyDiscoveryReply, 264);
SD_WIRE_ASSERT(DiscoveryReply, 308);
#undef SD_WIRE_ASSERT
static_assert(sizeof(char16_t) == 2 && sizeof(float) == 4);
static_assert(offsetof(IdentificationHeader, video_codec) == 36);
static_assert(offsetof(IdentificationHeader, refresh_rate) == 44);
static_assert(offsetof(IdentificationHeader, widths) == 52 && offsetof(IdentificationHeader, heights) == 88);
static_assert(offsetof(IdentificationHeader, license_type) == 124);
static_assert(offsetof(IdentificationData, device_name) == 78);
static_assert(offsetof(FramebufferHeader, update) == 24);
static_assert(offsetof(FramebufferHeader, compression) == 40 && offsetof(FramebufferHeader, chroma) == 44);
static_assert(offsetof(FramebufferHeader, quality) == 48 && offsetof(FramebufferHeader, four_fragments) == 64);
static_assert(offsetof(FramebufferHeader, fragment_count) == 60);
static_assert(offsetof(FramebufferHeader, fragment_payload_length) == 68);
static_assert(offsetof(PingHeader, option) == 12);
static_assert(offsetof(MouseHeader, button_flags) == 20);
static_assert(offsetof(KeyboardHeader, flags) == 16);
static_assert(offsetof(TouchHeader, flags) == 24 && offsetof(TouchHeader, contact_id) == 26);
static_assert(offsetof(TouchHeader, timestamp_ms) == 28 && offsetof(TouchHeader, additional) == 36);
static_assert(offsetof(TouchContact, flags) == 8 && offsetof(TouchContact, contact_id) == 10);
static_assert(offsetof(PenHeader, x) == 16 && offsetof(PenHeader, pressure) == 32);
static_assert(offsetof(PenHeader, contact) == 36 && offsetof(PenHeader, in_range) == 39);
static_assert(offsetof(LegacyDiscoveryReply, tcp_port) == 260);
static_assert(offsetof(DiscoveryReply, version_major) == 272);

template<class Packet> std::array<uint8_t, sizeof(Packet)> bytes(const Packet& packet) {
    static_assert(std::is_trivially_copyable_v<Packet> && std::is_standard_layout_v<Packet>);
    std::array<uint8_t, sizeof(Packet)> output{};
    std::memcpy(output.data(), &packet, output.size());
    return output;
}
template<class Packet, size_t Size> Packet decode(const std::array<uint8_t, Size>& input) {
    static_assert(sizeof(Packet) == Size && std::is_trivially_copyable_v<Packet>);
    Packet output{};
    std::memcpy(&output, input.data(), Size);
    return output;
}
}
