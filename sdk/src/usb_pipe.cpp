#include "usb_pipe.h"
#include <algorithm>

namespace sd::native {
SAUsbPipeStream::~SAUsbPipeStream() {
    Cancel();
    if(callbacks_.release) { try { callbacks_.release(callbacks_.context); } catch(...) {} }
}
void SAUsbPipeStream::Cancel() noexcept {
    if(!cancelled_.exchange(true)) { try { callbacks_.cancel(callbacks_.context); } catch(...) {} }
}
bool SAUsbPipeStream::ReadExact(uint8_t* bytes,uint32_t count,ULONGLONG& started) {
    uint32_t offset=0;
    while(offset<count) {
        if(cancelled_) return false;
        const auto now=GetTickCount64();
        if(started && now-started>=timeout_) throw Failure(SD_IO_ERROR,"Incomplete USB packet timed out");
        const uint32_t wait=started?uint32_t(std::min<ULONGLONG>(100,started+timeout_-now)):std::min(100u,timeout_);
        const uint32_t requested=std::min(count-offset,1024u*1024u);uint32_t transferred=0;
        const auto result=callbacks_.read(callbacks_.context,bytes+offset,requested,wait,&transferred);
        if(cancelled_) return false;
        if(transferred>requested || (result!=SD_OK && transferred) || (result==SD_OK && !transferred))
            throw Failure(SD_IO_ERROR,"USB read callback violated its transfer contract");
        if(result==SD_OK) { if(!started) started=GetTickCount64();offset+=transferred;continue; }
        if(result==SD_TIMEOUT) { Sleep(1);continue; }
        if(result==SD_DISCONNECTED) {
            if(started) throw Failure(SD_PROTOCOL_ERROR,"USB stream ended in the middle of a packet");
            return false;
        }
        throw Failure(result<0?result:SD_IO_ERROR,"USB stream read failed");
    }
    return true;
}
void SAUsbPipeStream::Send(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    const auto deadline=GetTickCount64()+timeout_;size_t offset=0;
    while(offset<bytes.size()) {
        if(cancelled_) throw Failure(SD_IO_ERROR,"USB stream was cancelled");
        const auto now=GetTickCount64();
        if(now>=deadline) throw Failure(SD_IO_ERROR,"USB stream write timed out");
        const auto request=uint32_t(std::min<size_t>(bytes.size()-offset,1024*1024));uint32_t transferred=0;
        const auto result=callbacks_.write(callbacks_.context,bytes.data()+offset,request,uint32_t(std::min<ULONGLONG>(100,deadline-now)),&transferred);
        if(transferred>request || (result!=SD_OK && transferred) || (result==SD_OK && !transferred))
            throw Failure(SD_IO_ERROR,"USB write callback violated its transfer contract");
        if(result==SD_OK) { offset+=transferred;continue; }
        if(result==SD_TIMEOUT) { Sleep(1);continue; }
        throw Failure(result<0?result:SD_IO_ERROR,"USB stream write failed");
    }
}
bool SAUsbPipeStream::Receive(SANetPacket& packet) {
    ULONGLONG started=0;
    if(!ReadExact(packet.header.data(),uint32_t(packet.header.size()),started)) return false;
    const auto h=protocol::decode<protocol::PacketHeader>(packet.header);
    const auto length=h.prefix.payload_length;const auto type=h.prefix.type;
    if(uint32_t(type)>=uint32_t(protocol::HeaderType::MAX) || length>max_payload ||
       (type!=protocol::HeaderType::FRAMEBUFFER && type!=protocol::HeaderType::AUDIO && length) ||
       (type==protocol::HeaderType::FRAMEBUFFER && !length) ||
       (type==protocol::HeaderType::AUDIO && (length>4*1024*1024 || length%4)))
        throw Failure(SD_PROTOCOL_ERROR,"Invalid USB packet type or payload length");
    if(type==protocol::HeaderType::VISIBILITY && h.value8>1)
        throw Failure(SD_PROTOCOL_ERROR,"Invalid USB visibility value");
    packet.payload.resize(length);
    return !length || ReadExact(packet.payload.data(),length,started);
}
}
