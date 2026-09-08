#include "sonar_profile_exchange.h"
#include <stdexcept>
#include <string_view>

namespace sd::native {
namespace {
bool equal_ascii_ignore_case(std::string_view a,std::string_view lower) {
    if(a.size()!=lower.size()) return false;
    for(size_t i=0;i<a.size();++i) {
        const char c=a[i]>='A'&&a[i]<='Z'?char(a[i]+('a'-'A')):a[i];
        if(c!=lower[i]) return false;
    }
    return true;
}
}
void SonarProfileExchange::SaveProfile(const std::string* profile) {
    auto edit=environment_.Edit();
    if(!profile) throw std::runtime_error("Null Sonar provider value");
    if(profile->empty()) edit->Remove(SonarProfileKey);else edit->PutString(SonarProfileKey,*profile);
    edit->PutBoolean(SonarLoadedKey,true);
    edit->Commit(); // Source ignores the boolean; ApplyProfile still follows false.
    apply_(*profile);
}
void SonarProfileExchange::Provider(SonarProfileProvider provider) noexcept {
    try {
        auto cursor=environment_.Query(provider);
        if(cursor) {
            if(cursor->Count()>0) {
                cursor->MoveFirst(); // Original return value is ignored.
                const auto text=cursor->Text();
                if(provider==SonarProfileProvider::Calibration) SaveProfile(text?&*text:nullptr);
                else {
                    auto edit=environment_.Edit();if(!text) throw std::runtime_error("Null Sonar provider value");
                    edit->PutBoolean(SonarBypassKey,!equal_ascii_ignore_case(*text,"no"));edit->Commit();
                }
            }
            cursor->Close(); // As in V/J, not a finally block after a thrown read/edit.
        }
    } catch(...) {}
}
void SonarProfileExchange::Bootstrap() {
    Provider(SonarProfileProvider::Calibration);
    Provider(SonarProfileProvider::Compatibility);
    environment_.RegisterReceiver(); // Original constructor does not catch registration failure.
}
bool SonarProfileExchange::Receive(const std::string& to,const std::string& sender,const std::string& profile) {
    const auto& application=environment_.ApplicationName();
    if((!equal_ascii_ignore_case(to,"all") || sender==application) && to!=application) return false;
    SaveProfile(&profile);return true; // g.e has no catch around edits/commit/u.
}
}
