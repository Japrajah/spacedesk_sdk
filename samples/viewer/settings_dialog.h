#pragma once
#include <windows.h>
#include "spacedesk_settings.hpp"
#include <memory>
#include <string>
#include <vector>

namespace sd::viewer {
struct SettingChoice { int64_t value; std::wstring label; };
struct AudioEndpointChoice { std::string id;std::wstring label; };
std::vector<AudioEndpointChoice> AudioEndpointChoices(uint32_t flow);
struct SettingField {
    sd_setting_info info{};
    std::wstring label, group;
    std::vector<SettingChoice> choices;
};

// A detached edit transaction. Only Commit changes the caller's settings.
class SettingsDraft {
    std::unique_ptr<SASettings> value_;
public:
    explicit SettingsDraft(const SASettings& source);
    SASettings& Value() { return *value_; }
    const SASettings& Value() const { return *value_; }
    void Set(const SettingField& field, const std::wstring& text);
    void Defaults();
    void Load(const std::wstring& path);
    void Save(const std::wstring& path, bool include_credentials) const;
    std::string Validate() const;
    void Commit(SASettings& destination) const;
};
std::vector<SettingField> SettingsFields();
std::wstring SettingSupport(const SettingField& field);
std::wstring SettingsWide(const std::string& text);
std::string SettingsUtf8(const std::wstring& text);
std::string ViewerShortcutError(const SASettings& settings);

// Owner thread only. true means validated draft committed atomically. A running
// session is never mutated; the caller applies the draft on its next connection.
bool EditSettings(HWND owner, SASettings& settings, bool connected = false,const char* initial_field = nullptr);

// Creates real, hidden Win32 controls, exercises their selection and reports
// layout/control invariants. Optional BMP captures WM_PRINT output, not a desktop.
bool SettingsDialogSmokeTest(const wchar_t* bitmap_path = nullptr,const char* focus_key = nullptr);
}
