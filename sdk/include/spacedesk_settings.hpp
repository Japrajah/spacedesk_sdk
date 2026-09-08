#pragma once
#include "spacedesk_sdk.h"
#include <stdexcept>
#include <string>
#include <utility>

namespace sd {
class SettingsError : public std::runtime_error {
public:
    sd_result result;
    explicit SettingsError(sd_result r) : std::runtime_error("SASettings operation failed"),result(r) {}
};
// C++ convenience owner; all storage and validation remain inside the DLL.
// Create/use/destroy on one thread, as with the C settings handle.
class SASettings {
    sd_settings* handle_=nullptr;
    static void Check(sd_result r) { if(r!=SD_OK) throw SettingsError(r); }
public:
    SASettings() { Check(sd_settings_create(&handle_)); }
    ~SASettings() { sd_settings_destroy(handle_); }
    SASettings(const SASettings&)=delete;
    SASettings& operator=(const SASettings&)=delete;
    SASettings(SASettings&& other) noexcept :handle_(std::exchange(other.handle_,nullptr)) {}
    SASettings& operator=(SASettings&&)=delete;
    sd_settings* Native() const noexcept { return handle_; }
    void SetInteger(const char* key,int64_t value) { Check(sd_settings_set_integer(handle_,key,value)); }
    int64_t GetInteger(const char* key) const { int64_t value=0;Check(sd_settings_get_integer(handle_,key,&value));return value; }
    void SetText(const char* key,const std::string& value) { Check(sd_settings_set_text(handle_,key,value.c_str())); }
    std::string GetText(const char* key) const {
        uint32_t size=0;Check(sd_settings_get_text(handle_,key,nullptr,0,&size));
        std::string result(size,'\0');Check(sd_settings_get_text(handle_,key,result.data(),size,&size));
        result.resize(size-1);return result;
    }
    std::string Serialize(uint32_t flags=0) const {
        uint32_t size=0;Check(sd_settings_serialize(handle_,flags,nullptr,0,&size));
        std::string result(size,'\0');Check(sd_settings_serialize(handle_,flags,result.data(),size,&size));
        result.resize(size-1);return result;
    }
    void Deserialize(const std::string& text) {
        if(text.size()>1024*1024) throw SettingsError(SD_INVALID_ARGUMENT);
        Check(sd_settings_deserialize(handle_,text.data(),uint32_t(text.size())));
    }
    sd_result Apply(sd_client* client) const { return sd_client_apply_settings(client,handle_); }
    void ReadFrom(sd_client* client) { Check(sd_client_get_settings(client,handle_)); }
};
}
