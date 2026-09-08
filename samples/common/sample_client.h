#pragma once
#include "sample_support.h"
#include <algorithm>
#include <array>
#include <chrono>

// A small C++ facade for the examples, NOT an additional SDK API.
// All network, codec and input packet handling stays inside the DLL.
namespace sample {

struct Host {
    std::string address;
    uint16_t port;
};

inline bool SameName(const char* utf8, const std::wstring& name) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (size <= 0) return false;
    std::wstring decoded(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, decoded.data(), size);
    decoded.pop_back();
    return CompareStringOrdinal(decoded.data(), int(decoded.size()), name.data(),
                                int(name.size()), TRUE) == CSTR_EQUAL;
}

inline Host SelectHost(const std::vector<sd_host>& hosts, std::istream& input = std::cin,
                       std::ostream& output = std::cout) {
    if (hosts.empty())
        throw std::runtime_error("No matching spacedesk hosts found. Check DRIVER/network or specify HOST.");

    size_t selected = 0;
    if (hosts.size() > 1) {
        for (size_t i = 0; i < hosts.size(); ++i)
            output << i + 1 << ") " << hosts[i].hostname << "  " << hosts[i].ipv4
                   << ':' << hosts[i].port << '\n';
        output << "Select host [1-" << hosts.size() << "]: " << std::flush;
        std::string answer;
        if (!std::getline(input, answer))
            throw UsageError("Host selection needs console input; set an IP or unique host name in main.cpp.");
        selected = size_t(Number(std::wstring(answer.begin(), answer.end()), 1, hosts.size()) - 1);
    }
    const auto& host = hosts[selected];
    output << "Host: " << host.hostname << " (" << host.ipv4 << ':' << host.port << ")\n";
    return {host.ipv4, uint16_t(host.port)};
}

// Explicit IP skips discovery. Otherwise search the LAN, optionally filter by
// advertised host name, then choose if several hosts match. A single match is automatic.
inline Host find_host(const std::wstring& address, const std::wstring& name, uint16_t port) {
    if (!address.empty()) {
        if (!name.empty()) throw UsageError("Set either the IP or the host name in find_host(), not both.");
        return {Utf8(address), port};
    }
    std::cout << "Discovering spacedesk hosts (1.5 seconds)...\n" << std::flush;
    std::array<sd_host, 64> found{};
    for (auto& host : found) host.struct_size = sizeof(host);
    uint32_t count = 0;
    Check(sd_discover(nullptr, port, 1500, found.data(), uint32_t(found.size()), &count),
          nullptr, "sd_discover");
    std::vector<sd_host> matches;
    for (uint32_t i = 0; i < count; ++i)
        if (name.empty() || SameName(found[i].hostname, name)) matches.push_back(found[i]);
    std::sort(matches.begin(), matches.end(), [](const sd_host& a, const sd_host& b) {
        const int order = std::string(a.hostname).compare(b.hostname);
        if (order) return order < 0;
        const int ip_order = std::string(a.ipv4).compare(b.ipv4);
        return ip_order ? ip_order < 0 : a.port < b.port;
    });
    return SelectHost(matches);
}

// A bounded event loop. Connecting / waiting for the first frame has a separate timeout.
class ConnectionLoop {
    ULONGLONG start_ = GetTickCount64();
    uint32_t duration_;
public:
    explicit ConnectionLoop(uint32_t duration_ms = 10000) : duration_(duration_ms) {}
    uint32_t elapsed_ms() const { return uint32_t(GetTickCount64() - start_); }
    bool running() const { return elapsed_ms() < duration_; }
    uint32_t poll_timeout() const {
        const auto elapsed = elapsed_ms();
        return elapsed < duration_ ? std::min(20u, duration_ - elapsed) : 0;
    }
};

enum class Touch { Down, Move, Up };

class Connection {
    Client handle_;
    uint32_t width_ = 0, height_ = 0;
    bool audio_ = false;

    void check(sd_result result, const char* operation) const {
        Check(result, handle_.value, operation);
    }
public:
    Connection() {
        sd_config config{};
        Check(sd_config_init(&config), nullptr, "sd_config_init");
        Check(sd_client_create(&config, &handle_.value), nullptr, "sd_client_create");
    }
    ~Connection() {
        if (audio_) sd_client_audio_stop(handle_.value);
        // handle_ then disconnects and destroys the display client on this thread.
    }
    void connect(const Host& host) {
        check(sd_client_connect(handle_.value, host.address.c_str(), host.port), "sd_client_connect");
    }
    sd_event poll(uint32_t timeout_ms = 20) { return Poll(handle_.value, timeout_ms); }
    sd_frame frame() {
        sd_frame result{};
        result.struct_size = sizeof(result);
        check(sd_client_get_frame(handle_.value, &result), "sd_client_get_frame");
        width_ = result.width;
        height_ = result.height;
        return result; // Borrowed pixels: consume/copy before the next poll.
    }
    sd_frame wait_frame(uint32_t timeout_ms = 10000) {
        WaitFrame(handle_.value, timeout_ms);
        return frame();
    }
    void mouse_move(int32_t x, int32_t y, bool absolute = false) {
        if (absolute) {
            if (x < 0 || y < 0 || uint32_t(x) >= width_ || uint32_t(y) >= height_)
                throw UsageError("Absolute point is outside the received frame");
            check(sd_client_send_mouse_absolute(handle_.value, float(x), float(y), width_, height_,
                                                0, SD_MOUSE_MOVE), "sd_client_send_mouse_absolute");
        } else {
            check(sd_client_send_mouse(handle_.value, x, y, 0, SD_MOUSE_MOVE, 0), "sd_client_send_mouse");
        }
    }
    void touch(Touch action, int32_t x, int32_t y) {
        if (x < 0 || y < 0 || uint32_t(x) >= width_ || uint32_t(y) >= height_)
            throw UsageError("Touch point is outside the received frame");
        sd_touch_event event{};
        event.struct_size = sizeof(event);
        event.surface_width = width_;
        event.surface_height = height_;
        event.timestamp_ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        event.contact_count = 1;
        // Classic Windows Mouse local (as touch): DOWN=3, MOVE=1, UP=5.
        // One continuous contact with ID 0, in the received display's pixel coordinates.
        const uint16_t flags = action == Touch::Down ? 3 : action == Touch::Up ? 5 : 1;
        event.contacts[0] = {x, y, flags, 0};
        check(sd_client_send_touch(handle_.value, &event), "sd_client_send_touch");
    }
    void key_press(uint32_t key) {
        const auto mapped = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
        const uint32_t flags = (mapped & 0xff00) == 0xe000 ? uint32_t(SD_KEY_EXTENDED) : 0;
        check(sd_client_send_keyboard(handle_.value, key, mapped & 0xff, flags), "keyboard DOWN");
        check(sd_client_send_keyboard(handle_.value, key, mapped & 0xff, flags | SD_KEY_UP), "keyboard UP");
    }
    void text(const std::wstring& text) {
        const std::vector<uint16_t> units(text.begin(), text.end());
        check(sd_client_send_text_utf16(handle_.value, units.data(), uint32_t(units.size()), 0),
              "sd_client_send_text_utf16");
    }
    void start_audio(bool play = false) {
        sd_audio_config config{};
        check(sd_audio_config_init(&config), "sd_audio_config_init");
        config.playback = play;
        config.capture_queue_ms = 250;
        config.probe_server = 0;
        const auto result = sd_client_audio_start(handle_.value, &config);
        if (result != SD_PENDING) check(result, "sd_client_audio_start");
        audio_ = true;
    }
    bool receive_audio(sd_audio_buffer& buffer, uint32_t timeout_ms) {
        buffer = {};
        buffer.struct_size = sizeof(buffer);
        const auto result = sd_client_audio_poll(handle_.value, timeout_ms, &buffer);
        if (result == SD_TIMEOUT) return false;
        if (result == SD_DISCONNECTED) {
            sd_audio_disconnect_state reason{};
            reason.struct_size = sizeof(reason);
            sd_client_audio_get_disconnect_state(handle_.value, &reason);
            throw std::runtime_error(reason.received ? "Audio server disconnected; reason " +
                std::to_string(reason.server_reason) : "Audio connection ended");
        }
        check(result, "sd_client_audio_poll");
        return buffer.frame_count != 0;
    }
};
} // namespace sample
