#pragma once
#include "spacedesk_sdk.h"
#include <string>
namespace sd::viewer {
// Display interpretation belongs to the frontend. Values are server b.j, not
// the client decoder b.i codes used by disconnect_with_reason.
inline std::wstring ServerDisconnectMessage(uint32_t reason) {
    switch(reason) {
    case SD_SERVER_DISCONNECT_REGULAR:return L"server disconnected";
    case SD_SERVER_DISCONNECT_NO_MEMORY:return L"server disconnected: out of memory";
    case SD_SERVER_DISCONNECT_UNSPECIFIED:return L"server disconnected: unspecified error";
    case SD_SERVER_DISCONNECT_H264_INIT_FAILED:return L"server H.264 encoder could not start - F9 opens encoding settings";
    case SD_SERVER_DISCONNECT_H264_ENCODE_FAILED:return L"server H.264 encoding failed - F9 opens encoding settings";
    default:return L"server disconnected: reason "+std::to_wstring(reason);
    }
}
inline bool IsServerDisconnect(const sd_event* event) noexcept {
    return event && event->type==SD_EVENT_DISCONNECTED && event->wire_type==8;
}
inline bool IsClientDisconnect(const sd_event* event) noexcept {
    return event && event->type==SD_EVENT_CLIENT_DISCONNECTED && !event->wire_type;
}
inline std::wstring ClientDisconnectMessage(uint32_t reason) {
    switch(reason) {
    case SD_DISCONNECT_H264_INIT_FAILED:return L"client H.264 decoder could not start - F9 opens encoding settings";
    case SD_DISCONNECT_H264_DECODE_FAILED:return L"client H.264 decoding failed - F9 opens encoding settings";
    default:return L"client disconnected: reason "+std::to_wstring(reason);
    }
}
inline bool DisconnectSuggestsEncoding(const sd_event* event) noexcept {
    return (IsServerDisconnect(event) && (event->value==SD_SERVER_DISCONNECT_H264_INIT_FAILED || event->value==SD_SERVER_DISCONNECT_H264_ENCODE_FAILED)) ||
        (IsClientDisconnect(event) && (event->value==SD_DISCONNECT_H264_INIT_FAILED || event->value==SD_DISCONNECT_H264_DECODE_FAILED));
}
}
