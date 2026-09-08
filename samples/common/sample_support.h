#pragma once
#include <windows.h>
#include "spacedesk_sdk.h"
#include <cerrno>
#include <cwchar>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace sample {
struct UsageError:std::runtime_error { using std::runtime_error::runtime_error; };
inline std::string Utf8(const std::wstring& text) {
    if(text.empty()) return {};
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if(!size) throw UsageError("Invalid Unicode argument");
    std::string result(size,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),size,nullptr,nullptr);return result;
}
inline int64_t Number(const std::wstring& text,int64_t minimum,int64_t maximum) {
    const size_t sign=!text.empty() && (text[0]==L'-'||text[0]==L'+')?1:0;
    const int radix=text.size()>sign+2 && text[sign]==L'0' && (text[sign+1]==L'x'||text[sign+1]==L'X')?16:10;
    wchar_t* end=nullptr;errno=0;const auto value=std::wcstoll(text.c_str(),&end,radix);
    if(text.empty() || end==text.c_str() || *end || errno==ERANGE || value<minimum || value>maximum) throw UsageError("Numeric argument is invalid or outside its supported range");
    return value;
}
inline void Check(sd_result result,sd_client* client,const char* operation) {
    if(result==SD_OK) return;
    const char* detail=client?sd_client_last_error(client):nullptr;
    throw std::runtime_error(std::string(operation)+" failed ("+std::to_string(result)+")"+(detail&&*detail?": "+std::string(detail):""));
}
struct Client {
    sd_client* value=nullptr;
    ~Client() { if(value) { sd_client_disconnect(value);sd_client_destroy(value); } }
    Client()=default;Client(const Client&)=delete;Client& operator=(const Client&)=delete;
};
inline sd_event Poll(sd_client* client,uint32_t timeout) {
    sd_event event{};event.struct_size=sizeof(event);
    const auto result=sd_client_poll(client,timeout,&event);
    if(result==SD_TIMEOUT) return event;
    if(result==SD_DISCONNECTED) {
        std::string message="Display disconnected";
        if(event.wire_type==8) {
            const auto name=sd_server_disconnect_reason_name(event.value);
            message+="; server reason "+std::to_string(event.value);if(name) message+=" ("+std::string(name)+")";
        }
        throw std::runtime_error(message);
    }
    Check(result,client,"sd_client_poll");return event;
}
inline sd_frame WaitFrame(sd_client* client,uint32_t timeout=10000) {
    const auto deadline=GetTickCount64()+timeout;
    while(GetTickCount64()<deadline) {
        const auto event=Poll(client,100);
        if(event.type==SD_EVENT_FRAME) { sd_frame frame{};frame.struct_size=sizeof(frame);Check(sd_client_get_frame(client,&frame),client,"sd_client_get_frame");return frame; }
    }
    throw std::runtime_error("Timed out waiting for a complete display frame");
}
class File {
    HANDLE handle_=INVALID_HANDLE_VALUE;
public:
    explicit File(const std::wstring& path) {
        handle_=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle_==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create output file; it may already exist (Windows error "+std::to_string(GetLastError())+")");
    }
    ~File() { if(handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    File(const File&)=delete;File& operator=(const File&)=delete;
    void Write(const void* bytes,uint32_t count) { DWORD written=0;if(!WriteFile(handle_,bytes,count,&written,nullptr)||written!=count) throw std::runtime_error("Output file write failed"); }
    void Seek(uint32_t offset) { LARGE_INTEGER at{};at.QuadPart=offset;if(!SetFilePointerEx(handle_,at,nullptr,FILE_BEGIN)) throw std::runtime_error("Output file seek failed"); }
};
template<class F> int Run(F&& body) {
    SetConsoleOutputCP(CP_UTF8);
    try { body();return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
}
