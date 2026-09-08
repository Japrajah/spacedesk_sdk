#pragma once
#include "frame_decoder.h"
#include <string>
#include <variant>
#include <vector>

namespace sd::native {
struct SettingDefinition {
    const char* key;
    uint32_t type;
    const char* initial;
    int64_t minimum, maximum;
    const char* allowed;
    uint32_t application;
    const char* apk;
};
// Independent per-client settings model. Android s2 is a Context singleton;
// the SDK deliberately has no Android Context/global shared preferences.
class SASettings {
    using Value = std::variant<int64_t, std::string>;
    std::vector<Value> values_;
    static size_t Index(const char*);
public:
    SASettings();
    static size_t Count();
    static const SettingDefinition& Definition(size_t);
    int64_t Integer(const char*) const;
    const std::string& Text(const char*) const;
    void Set(const char*, int64_t);
    void Set(const char*, const std::string&);
    std::vector<sd_resolution> Resolutions() const;
    sd_mouse_target MouseTarget() const;
    void SonarOptions(sd_sonar_options&,sd_sonar_profile_options&) const;
    void ValidateForClient() const;
    std::string Serialize(uint32_t flags) const;
    void Deserialize(const char*, uint32_t bytes);
    void Swap(SASettings& other) noexcept { values_.swap(other.values_); }
};
}
struct sd_settings {
    DWORD owner = GetCurrentThreadId();
    sd::native::SASettings value;
};
