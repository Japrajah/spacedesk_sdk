#include "websocket.h"
#include "frame_decoder.h"
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <map>
#include <string_view>

namespace sd::native {
namespace {
void random_bytes(uint8_t* output, ULONG count) {
    if (BCryptGenRandom(nullptr, output, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw Failure(SD_INTERNAL_ERROR, "WebSocket random generation failed");
}
std::string base64(const uint8_t* data, size_t count) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < count; i += 3) {
        const uint32_t value = uint32_t(data[i]) << 16 |
            (i + 1 < count ? uint32_t(data[i + 1]) << 8 : 0) | (i + 2 < count ? data[i + 2] : 0);
        result += alphabet[value >> 18]; result += alphabet[(value >> 12) & 63];
        result += i + 1 < count ? alphabet[(value >> 6) & 63] : '=';
        result += i + 2 < count ? alphabet[value & 63] : '=';
    }
    return result;
}
std::string accept_value(const std::string& key) {
    std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    struct Provider {
        BCRYPT_ALG_HANDLE handle = nullptr;
        ~Provider() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } provider;
    if (BCryptOpenAlgorithmProvider(&provider.handle, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
        throw Failure(SD_INTERNAL_ERROR, "WebSocket SHA-1 provider unavailable");
    std::array<uint8_t, 20> hash{};
    if (BCryptHash(provider.handle, nullptr, 0, reinterpret_cast<PUCHAR>(input.data()),
                   ULONG(input.size()), hash.data(), ULONG(hash.size())) < 0)
        throw Failure(SD_INTERNAL_ERROR, "WebSocket accept hash failed");
    return base64(hash.data(), hash.size());
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
std::string lower(std::string_view s) {
    std::string result(s);
    for (char& ch : result) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return result;
}
bool has_token(std::string_view value, std::string_view token) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        if (lower(trim(value.substr(0, comma))) == token) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}
}
std::string SNProtocolWebSocket::ClientKeyCreate() {
    std::array<uint8_t, 16> random{}; random_bytes(random.data(), ULONG(random.size()));
    return base64(random.data(), random.size());
}
std::string SNProtocolWebSocket::HandshakeRequestCreate(const std::string& host, uint16_t port, const std::string& key) {
    // Exact GET / HTTP prefix and Sec-WebSocket-Key spelling are required by
    // SNSocketTcpConnected::ReceiveIdentification / the legacy service parser.
    return "GET / HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
        "\r\nSec-WebSocket-Version: 13\r\n\r\n";
}
void SNProtocolWebSocket::HandshakeResponseValidate(const std::string& response, const std::string& key) {
    const auto first = response.find("\r\n");
    const auto status = std::string_view(response).substr(0, first);
    if (first == std::string::npos || (status != "HTTP/1.1 101" && status.substr(0, 13) != "HTTP/1.1 101 "))
        throw Failure(SD_PROTOCOL_ERROR, "WebSocket HTTP upgrade was not accepted");
    std::map<std::string, std::string> headers;
    size_t at = first + 2;
    while (at < response.size()) {
        const auto end = response.find("\r\n", at);
        if (end == std::string::npos) throw Failure(SD_PROTOCOL_ERROR, "Incomplete WebSocket HTTP headers");
        if (end == at) {
            if (end + 2 != response.size()) throw Failure(SD_PROTOCOL_ERROR, "Unexpected HTTP upgrade tail");
            break;
        }
        const std::string_view line(response.data() + at, end - at);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 || line.front() == ' ' || line.front() == '\t')
            throw Failure(SD_PROTOCOL_ERROR, "Invalid WebSocket HTTP header");
        const auto name = lower(line.substr(0, colon));
        const auto value = trim(line.substr(colon + 1));
        auto [it, inserted] = headers.emplace(name, value);
        if (!inserted) {
            if (name != "connection" && name != "upgrade")
                throw Failure(SD_PROTOCOL_ERROR, "Duplicate WebSocket HTTP header");
            it->second += ","; it->second += value;
        }
        at = end + 2;
    }
    if (!has_token(headers["upgrade"], "websocket") || !has_token(headers["connection"], "upgrade") ||
        headers["sec-websocket-accept"] != accept_value(key))
        throw Failure(SD_PROTOCOL_ERROR, "Invalid WebSocket upgrade accept or headers");
    if (headers.count("sec-websocket-extensions") || headers.count("sec-websocket-protocol"))
        throw Failure(SD_PROTOCOL_ERROR, "Unrequested WebSocket extension or subprotocol");
}
std::vector<uint8_t> SNProtocolWebSocket::ClientFrameCreate(uint8_t opcode, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> result;
    result.reserve(payload.size() + 14);
    result.push_back(0x80 | opcode);
    if (payload.size() <= 125) result.push_back(uint8_t(0x80 | payload.size()));
    else if (payload.size() <= 65535) {
        result.push_back(0xFE);
        result.push_back(uint8_t(payload.size() >> 8)); result.push_back(uint8_t(payload.size()));
    } else {
        result.push_back(0xFF);
        for (int shift = 56; shift >= 0; shift -= 8) result.push_back(uint8_t(uint64_t(payload.size()) >> shift));
    }
    std::array<uint8_t, 4> mask{}; random_bytes(mask.data(), ULONG(mask.size()));
    result.insert(result.end(), mask.begin(), mask.end());
    for (size_t i = 0; i < payload.size(); ++i) result.push_back(payload[i] ^ mask[i % 4]);
    return result;
}
void SNProtocolWebSocket::ClosePayloadValidate(const std::vector<uint8_t>& payload) {
    if (payload.size() == 1) throw Failure(SD_PROTOCOL_ERROR, "Truncated WebSocket close code");
    if (payload.empty()) return;
    const unsigned code = unsigned(payload[0]) << 8 | payload[1];
    if (!((code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) ||
          (code >= 3000 && code <= 4999))) throw Failure(SD_PROTOCOL_ERROR, "Invalid WebSocket close code");
    if (payload.size() > 2 && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(payload.data() + 2), int(payload.size() - 2), nullptr, 0))
        throw Failure(SD_PROTOCOL_ERROR, "Invalid UTF-8 in WebSocket close reason");
}
}
