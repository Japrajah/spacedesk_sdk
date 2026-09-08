#include "sonar_preferences.h"
#include "frame_decoder.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <cstring>
#include <objbase.h>

namespace sd::native {
namespace {
struct PreferenceFileHeader {
    char magic[8]={'S','D','S','P','R','E','F','1'};
    uint32_t version=1,flags=0,application_bytes=0,profile_bytes=0;
};
static_assert(sizeof(PreferenceFileHeader)==24);
void valid_utf8(const std::string& text) {
    if(!text.empty() && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0))
        throw Failure(SD_IO_ERROR,"Invalid UTF-8 Sonar preference text");
}
std::optional<std::string> read_file(const std::wstring& path,size_t limit) {
    if(path.empty()) return {};
    const auto file=std::filesystem::path(path);
    std::error_code error;const auto exists=std::filesystem::exists(file,error);
    if(error) throw Failure(SD_IO_ERROR,"Cannot inspect Sonar preference file");
    if(!exists) return {};
    const auto size=std::filesystem::file_size(file,error);
    if(error || size>limit) throw Failure(SD_IO_ERROR,"Invalid Sonar preference file size");
    std::ifstream stream(file,std::ios::binary);std::string data(size_t(size),'\0');
    if(!stream || (size && !stream.read(data.data(),std::streamsize(size))) || stream.peek()!=EOF)
        throw Failure(SD_IO_ERROR,"Cannot read Sonar preference file");
    return data;
}
struct CaseInsensitive {
    bool operator()(const std::wstring& a,const std::wstring& b) const {
        return CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_LESS_THAN;
    }
};
}
struct SonarPreferenceStorage {
    mutable std::mutex mutex;
    std::wstring path;
    std::string application,profile;
    uint32_t flags=0;
    sd_result result=SD_OK;
    void Load() {
        const auto data=read_file(path,24+127+65536);if(!data) return;
        PreferenceFileHeader header{},expected{};
        if(data->size()<sizeof(header)) throw Failure(SD_IO_ERROR,"Truncated Sonar preference store");
        std::memcpy(&header,data->data(),sizeof(header));
        if(std::memcmp(header.magic,expected.magic,8) || header.version!=1 || (header.flags&~15u) ||
           header.application_bytes>127 || header.profile_bytes>65536 ||
           data->size()!=24ull+header.application_bytes+header.profile_bytes ||
           data->substr(24,header.application_bytes)!=application || (!(header.flags&SD_SONAR_PROFILE_PRESENT) && header.profile_bytes))
            throw Failure(SD_IO_ERROR,"Invalid Sonar preference store or application identity");
        profile=data->substr(24+header.application_bytes,header.profile_bytes);valid_utf8(profile);flags=header.flags;
    }
    sd_result Save() noexcept {
        if(path.empty()) return SD_OK;
        std::wstring temporary;
        HANDLE handle=INVALID_HANDLE_VALUE;
        bool created=false;
        try {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            GUID guid{};if(FAILED(CoCreateGuid(&guid))) throw Failure(SD_IO_ERROR,"Cannot create Sonar store temporary ID");
            wchar_t suffix[40]{};StringFromGUID2(guid,suffix,40);temporary=path+L"."+suffix+L".tmp";
            handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
            if(handle==INVALID_HANDLE_VALUE) throw Failure(SD_IO_ERROR,"Cannot create Sonar store temporary file");
            created=true;
            PreferenceFileHeader header{};header.flags=flags;header.application_bytes=uint32_t(application.size());header.profile_bytes=uint32_t(profile.size());
            const auto write=[&](const void* bytes,DWORD size) { DWORD written=0;return !size || (WriteFile(handle,bytes,size,&written,nullptr) && written==size); };
            if(!write(&header,sizeof(header)) || !write(application.data(),uint32_t(application.size())) ||
               !write(profile.data(),uint32_t(profile.size())) || !FlushFileBuffers(handle)) throw Failure(SD_IO_ERROR,"Cannot write Sonar store");
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
            if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw Failure(SD_IO_ERROR,"Cannot replace Sonar store");
            return SD_OK;
        } catch(...) {
            if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
            if(created) DeleteFileW(temporary.c_str());
            return SD_IO_ERROR;
        }
    }
};
namespace {
std::mutex stores_mutex;
std::map<std::wstring,std::weak_ptr<SonarPreferenceStorage>,CaseInsensitive> stores;
std::shared_ptr<SonarPreferenceStorage> acquire(const SonarPreferenceConfig& config) {
    std::lock_guard<std::mutex> guard(stores_mutex);
    for(auto it=stores.begin();it!=stores.end();) if(it->second.expired()) it=stores.erase(it);else ++it;
    const auto key=config.store.empty()?std::wstring():std::filesystem::weakly_canonical(config.store).wstring();
    if(!key.empty()) {
        if(auto existing=stores[key].lock()) {
            if(existing->application!=config.application) throw Failure(SD_INVALID_ARGUMENT,"Sonar store is already owned by another application identity");
            return existing;
        }
    }
    auto storage=std::make_shared<SonarPreferenceStorage>();storage->application=config.application;storage->path=key;storage->Load();
    if(!key.empty()) stores[key]=storage;return storage;
}
class FileCursor final:public SonarProfileCursor {
    std::string text_;
public:
    explicit FileCursor(std::string text):text_(std::move(text)) {}
    int Count() override { return 1; }
    bool MoveFirst() override { return true; }
    std::optional<std::string> Text() override { return text_; }
    void Close() override {} // File bytes were already read and the file handle closed.
};
}
class SonarPreferences::Editor final:public SonarPreferenceEditor {
    SonarPreferences& owner_;
    std::vector<std::pair<std::string,Value>> writes_;
public:
    explicit Editor(SonarPreferences& owner):owner_(owner) {}
    void PutString(const char* key,const std::string& value) override { writes_.emplace_back(key,value); }
    void PutBoolean(const char* key,bool value) override { writes_.emplace_back(key,value); }
    void Remove(const char* key) override { writes_.emplace_back(key,std::monostate{}); }
    bool Commit() override { return owner_.Commit(writes_); }
};
SonarPreferences::SonarPreferences(SonarPreferenceConfig config,std::function<void(sd_result,const std::string&)> report)
    :config_(std::move(config)),storage_(acquire(config_)),report_(std::move(report)) {}
std::unique_ptr<SonarProfileCursor> SonarPreferences::Query(SonarProfileProvider provider) {
    auto text=read_file(provider==SonarProfileProvider::Calibration?config_.calibration:config_.compatibility,65536);
    if(!text) return {};valid_utf8(*text);return std::make_unique<FileCursor>(std::move(*text));
}
std::unique_ptr<SonarPreferenceEditor> SonarPreferences::Edit() { return std::make_unique<Editor>(*this); }
bool SonarPreferences::Commit(const std::vector<std::pair<std::string,Value>>& writes) {
    sd_result result;
    {
        std::lock_guard<std::mutex> guard(storage_->mutex);
        auto profile=storage_->profile;auto flags=storage_->flags;
        for(const auto& [key,value]:writes) {
            if(key==SonarProfileKey) {
                if(std::holds_alternative<std::monostate>(value)) { flags&=~SD_SONAR_PROFILE_PRESENT;profile.clear(); }
                else { profile=std::get<std::string>(value);flags|=SD_SONAR_PROFILE_PRESENT; }
            } else {
                const uint32_t flag=key==SonarLoadedKey?SD_SONAR_PROFILE_LOADED:key==SonarBypassKey?SD_SONAR_PROFILE_BYPASS:key==SonarLowFrequencyKey?SD_SONAR_PROFILE_LOW_FREQUENCY:0;
                if(!flag) throw Failure(SD_INVALID_ARGUMENT,"Unknown Sonar preference key");
                if(std::get<bool>(value)) flags|=flag;else flags&=~flag;
            }
        }
        storage_->profile=std::move(profile);storage_->flags=flags;result=storage_->result=storage_->Save();
    }
    if(result<0 && report_) report_(result,"Sonar preference write failed; in-memory update retained");
    return result==SD_OK;
}
SonarPreferenceSnapshot SonarPreferences::Snapshot() const {
    std::lock_guard<std::mutex> guard(storage_->mutex);
    return {storage_->flags|(registered_?SD_SONAR_PROFILE_RECEIVER:0u)|(!config_.store.empty()?SD_SONAR_PROFILE_PERSISTENT:0u),storage_->profile,storage_->result};
}
uint32_t SonarPreferences::Flags() const {
    std::lock_guard<std::mutex> guard(storage_->mutex);
    return storage_->flags|(registered_?SD_SONAR_PROFILE_RECEIVER:0u)|(!config_.store.empty()?SD_SONAR_PROFILE_PERSISTENT:0u);
}
void SonarPreferences::SetProfile(const std::string& text) {
    auto edit=Edit();if(text.empty()) edit->Remove(SonarProfileKey);else edit->PutString(SonarProfileKey,text);
    edit->PutBoolean(SonarLoadedKey,true);edit->Commit();
}
void SonarPreferences::SaveLowFrequency() { auto edit=Edit();edit->PutBoolean(SonarLowFrequencyKey,true);edit->Commit(); }
}
