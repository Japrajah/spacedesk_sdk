#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sd::native {
// Service class name recovered from diagnostics. Client-side counterparts for
// its handshake and framing; this is not an inferred UWP inheritance relation.
class SNProtocolWebSocket {
public:
    static std::string ClientKeyCreate();
    static std::string HandshakeRequestCreate(const std::string& host, uint16_t port, const std::string& key);
    static void HandshakeResponseValidate(const std::string& response, const std::string& key);
    static std::vector<uint8_t> ClientFrameCreate(uint8_t opcode, const std::vector<uint8_t>& payload);
    static void ClosePayloadValidate(const std::vector<uint8_t>& payload);
};
}
