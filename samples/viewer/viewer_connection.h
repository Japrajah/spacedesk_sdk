#pragma once
#include "spacedesk_settings.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace sd::viewer {
enum class ConnectionTargetMode { Direct, DiscoverIp, DiscoverHostname, DiscoverAny };

// A detached request: the worker never reads or mutates the UI's SASettings.
// DiscoverIp sends unicast discovery; DiscoverHostname uses broadcast discovery
// and compares the advertised name exactly, without consulting DNS.
struct ConnectionTarget {
    ConnectionTargetMode mode = ConnectionTargetMode::Direct;
    std::string target;
    uint16_t port = 28252;
    bool encrypted = false;
};
struct DiscoveredConnection {
    std::string ipv4;
    uint16_t port = 28252;
    uint32_t feature_flags = 0;
};

inline bool ConnectionHasTransport(uint32_t state) noexcept {
    return state == SD_STATE_DISPLAY_OFF || state == SD_STATE_DISPLAY_ON || state == SD_STATE_FULLSCREEN;
}
inline bool ConnectionInvalidatesFrame(uint32_t state) noexcept {
    // Disconnected, connecting, reconnecting and disconnecting have no usable
    // borrowed frame. Unknown future states are treated conservatively too.
    return !ConnectionHasTransport(state);
}

inline bool IsConnectionIpv4(const std::string& text) {
    // Canonical dotted decimal only, matching the SDK's numeric IPv4 input.
    // Reject shortened/octal/hex forms, signs, leading zeroes and whitespace.
    size_t cursor = 0;
    for (unsigned octet = 0; octet != 4; ++octet) {
        const auto start = cursor;
        unsigned value = 0;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            if (cursor - start == 3) return false;
            value = value * 10 + unsigned(text[cursor++] - '0');
        }
        if (cursor == start || value > 255 || (cursor - start > 1 && text[start] == '0')) return false;
        if (octet == 3) return cursor == text.size();
        if (cursor == text.size() || text[cursor++] != '.') return false;
    }
    return false;
}

inline ConnectionTarget MakeConnectionTarget(const SASettings& settings,
    const std::string& explicit_ip = {}, uint16_t port = 28252) {
    ConnectionTarget result;
    if(settings.GetInteger("connection.transport")!=0)
        throw std::runtime_error("This viewer opens network targets. A USB profile needs an application-provided peripheral stream adapter.");
    result.port = port ? port : 28252;
    result.encrypted = settings.GetInteger("security.encryption") != 0;
    const bool discovery = settings.GetInteger("connection.discovery") != 0;
    if (!explicit_ip.empty()) result.target = explicit_ip;
    else {
        switch (settings.GetInteger("connection.auto_connect")) {
        case 0:
            // User-requested viewer default: with no supplied/configured target,
            // discover a server instead of requiring command-line arguments.
            result.mode=ConnectionTargetMode::DiscoverAny;
            return result;
        case 1:
            result.target = settings.GetText("connection.ip");
            break;
        case 2:
            result.target = settings.GetText("connection.hostname");
            if (result.target.empty()) throw std::runtime_error("Enter the server's advertised name in Server host name.");
            if (!discovery) throw std::runtime_error("Enable Discover servers on local network to connect by advertised host name; this viewer does not resolve names through DNS.");
            result.mode = ConnectionTargetMode::DiscoverHostname;
            return result;
        default:
            throw std::runtime_error("Unknown Automatic connection mode.");
        }
    }
    if (!IsConnectionIpv4(result.target))
        throw std::runtime_error("Enter a server IPv4 address as four decimal numbers, for example 192.168.1.5.");
    result.mode = discovery ? ConnectionTargetMode::DiscoverIp : ConnectionTargetMode::Direct;
    return result;
}

namespace connection_detail {
template<size_t N> inline std::optional<std::string> BoundedText(const char (&text)[N]) {
    size_t size = 0;
    while (size != N && text[size]) ++size;
    if (size == N) return std::nullopt;
    return std::string(text, size);
}
}

// No match is normal while discovery is in progress. Invalid records are
// skipped. A matching server that requires encryption produces a diagnostic;
// never continue with another plaintext server or downgrade the request.
inline std::optional<DiscoveredConnection> MatchDiscovery(const ConnectionTarget& target,
    const sd_host* hosts, size_t count) {
    if (target.mode == ConnectionTargetMode::Direct) return std::nullopt;
    if (!hosts && count) throw std::invalid_argument("Missing discovery results.");
    for (size_t i = 0; i < count; ++i) {
        const auto& host = hosts[i];
        if (host.struct_size != sizeof(sd_host) || !host.port || host.port > 65535) continue;
        const auto address = connection_detail::BoundedText(host.ipv4);
        const auto name = connection_detail::BoundedText(host.hostname);
        if (!address || !IsConnectionIpv4(*address) || !name) continue;
        const bool matches = target.mode==ConnectionTargetMode::DiscoverAny ||
            (target.mode == ConnectionTargetMode::DiscoverIp ? *address == target.target : *name == target.target);
        if (!matches) continue;
        if ((host.feature_flags & SD_SERVER_FEATURE_ENCRYPTION) && !target.encrypted)
            throw std::runtime_error("This server requires encryption. Select TLS 1.2 or TLS 1.3 in Encryption and configure its PIN or PSK before connecting.");
        return DiscoveredConnection{*address, uint16_t(host.port), host.feature_flags};
    }
    return std::nullopt;
}

// Owner-thread gate for detached jobs. Workers only carry a copied token; the
// owner checks Current before consuming their result. Cancel does not block a
// worker, but invalidates its result immediately. Generation zero is never live.
class GenerationGate {
    uint64_t generation_ = 0;
    bool active_ = false;
public:
    uint64_t Begin() {
        if (generation_ == (std::numeric_limits<uint64_t>::max)())
            throw std::overflow_error("Connection request generation exhausted.");
        active_ = true;
        return ++generation_;
    }
    void Cancel() noexcept { active_ = false; }
    bool Current(uint64_t token) const noexcept { return active_ && token != 0 && token == generation_; }
};
}
