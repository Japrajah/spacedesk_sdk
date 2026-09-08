#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace sd::native {
inline constexpr const char* SonarProfileKey="__sonarpen_profile";
inline constexpr const char* SonarLoadedKey="__sonarpen_loaded";
inline constexpr const char* SonarBypassKey="__sonarpen_bypass";
inline constexpr const char* SonarLowFrequencyKey="lowFequence";
enum class SonarProfileProvider { Calibration,Compatibility };
// Host boundaries for the original Cursor/SharedPreferences/Context calls.
// These interfaces are SDK adapters, not recovered APK superclasses.
struct SonarProfileCursor {
    virtual ~SonarProfileCursor()=default;
    virtual int Count()=0;
    virtual bool MoveFirst()=0;
    virtual std::optional<std::string> Text()=0;
    virtual void Close()=0;
};
struct SonarPreferenceEditor {
    virtual ~SonarPreferenceEditor()=default;
    virtual void PutString(const char*,const std::string&)=0;
    virtual void PutBoolean(const char*,bool)=0;
    virtual void Remove(const char*)=0;
    virtual bool Commit()=0;
};
struct SonarProfileEnvironment {
    virtual ~SonarProfileEnvironment()=default;
    virtual const std::string& ApplicationName() const=0;
    virtual std::unique_ptr<SonarProfileCursor> Query(SonarProfileProvider)=0;
    virtual std::unique_ptr<SonarPreferenceEditor> Edit()=0;
    virtual void RegisterReceiver()=0;
};
// Role alias for g.V/J and nested g.e.onReceive; no inferred native class name.
class SonarProfileExchange {
    SonarProfileEnvironment& environment_;
    std::function<void(const std::string&)> apply_;
    void Provider(SonarProfileProvider) noexcept;
    void SaveProfile(const std::string*);
public:
    SonarProfileExchange(SonarProfileEnvironment& environment,std::function<void(const std::string&)> apply)
        :environment_(environment),apply_(std::move(apply)) {}
    void Bootstrap();
    bool Receive(const std::string& to,const std::string& sender,const std::string& profile);
};
}
