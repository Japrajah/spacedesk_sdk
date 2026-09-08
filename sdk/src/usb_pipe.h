#pragma once
#include "packet.h"
#include "frame_decoder.h"
#include <atomic>
#include <mutex>

namespace sd::native {
// Log-confirmed SAUsbPipeStream (APK r2.o). No recovered inheritance or VMT.
class SAUsbPipeStream {
    sd_usb_stream callbacks_{};
    uint32_t timeout_;
    std::atomic<bool> cancelled_{false};
    std::mutex send_mutex_;
    bool ReadExact(uint8_t*,uint32_t,ULONGLONG&);
public:
    SAUsbPipeStream(const sd_usb_stream& callbacks,uint32_t timeout):callbacks_(callbacks),timeout_(timeout) {}
    ~SAUsbPipeStream();
    void Cancel() noexcept;
    void Send(const std::vector<uint8_t>&);
    bool Receive(SANetPacket&);
};
}
