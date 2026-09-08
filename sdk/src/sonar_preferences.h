#pragma once
#include "sonar_profile_exchange.h"
#include "spacedesk_sdk.h"
#include <mutex>
#include <vector>
#include <variant>

namespace sd::native {
struct SonarPreferenceConfig {
    std::string application="spacedesk.sdk";
    std::wstring store,calibration,compatibility;
};
struct SonarPreferenceSnapshot { uint32_t flags=0;std::string profile;sd_result result=SD_OK; };
struct SonarPreferenceStorage;
// Windows file/memory adapter; its binary file format is SDK-owned, not Android XML.
class SonarPreferences final:public SonarProfileEnvironment {
    using Value=std::variant<std::monostate,bool,std::string>;
    class Editor;
    SonarPreferenceConfig config_;
    std::shared_ptr<SonarPreferenceStorage> storage_;
    std::function<void(sd_result,const std::string&)> report_;
    bool registered_=false;
    bool Commit(const std::vector<std::pair<std::string,Value>>&);
public:
    SonarPreferences(SonarPreferenceConfig,std::function<void(sd_result,const std::string&)>);
    const std::string& ApplicationName() const override { return config_.application; }
    std::unique_ptr<SonarProfileCursor> Query(SonarProfileProvider) override;
    std::unique_ptr<SonarPreferenceEditor> Edit() override;
    void RegisterReceiver() override { registered_=true; }
    SonarPreferenceSnapshot Snapshot() const;
    uint32_t Flags() const;
    void SetProfile(const std::string&);
    void SaveLowFrequency();
};
}
