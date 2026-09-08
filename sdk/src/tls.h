#pragma once
#include <winsock2.h>
#include "frame_decoder.h"
#include <memory>
#include <mutex>
#include <string>

namespace sd::native {
class SASettings;
struct SAPreSharedKey {
    std::vector<uint8_t> bytes;
    ~SAPreSharedKey() { if(!bytes.empty()) SecureZeroMemory(bytes.data(),bytes.size()); }
};
struct SASecureSocketLayerConfig {
    uint32_t mode=SD_TLS_NONE;
    std::string identity="Client_identity";
    std::shared_ptr<const SAPreSharedKey> key;
    static SASecureSocketLayerConfig FromSettings(const SASettings&);
};
enum class SATlsProgress { Complete, Read, Write, Closed };
struct SATlsIo { size_t count=0; SATlsProgress progress=SATlsProgress::Complete; };
// SDK RAII boundary around the native SAOpenSSL.cpp ContextCreate/ConnectionConnect
// helpers. The original helpers do not have a class VMT; no inheritance is invented.
class SASecureSocketLayer {
    struct Impl;
    mutable std::mutex mutex_;
    SASecureSocketLayerConfig config_;
    sd_tls_state state_{};
    std::unique_ptr<Impl> impl_;
public:
    explicit SASecureSocketLayer(SASecureSocketLayerConfig config);
    ~SASecureSocketLayer();
    void Initialize(SOCKET);
    SATlsProgress Connect();
    SATlsIo Read(uint8_t*,size_t);
    SATlsIo Write(const uint8_t*,size_t);
    SATlsProgress Shutdown();
    ULONGLONG IncompleteRecordSince() const;
    void StateGet(sd_tls_state&) const;
};
}
