#include "settings.h"
#include "client_identity.h"
#include <filesystem>
#include "tls.h"
#include "absolute_mouse.h"
#include <charconv>
#include <cstring>
#include <set>
#include <objbase.h>

namespace sd::native {
namespace {
constexpr uint32_t INTEGER=SD_SETTING_INTEGER, BOOLEAN=SD_SETTING_BOOLEAN, TEXT=SD_SETTING_TEXT;
constexpr uint32_t ENGINE=SD_SETTING_ENGINE, FRONTEND=SD_SETTING_FRONTEND, PENDING=SD_SETTING_PENDING;
constexpr uint32_t SECRET_ENGINE=SD_SETTING_ENGINE|SD_SETTING_SECRET;
const SettingDefinition schema[] = {
#define S(key,type,initial,min,max,allowed,application,apk) {key,type,initial,min,max,allowed,application,apk},
#include "settings_schema.inc"
#undef S
};
int64_t number(const std::string& s) {
    int64_t v=0; const auto result=std::from_chars(s.data(),s.data()+s.size(),v);
    if (result.ec!=std::errc() || result.ptr!=s.data()+s.size())
        throw Failure(SD_INVALID_ARGUMENT,"Setting requires a decimal integer");
    return v;
}
std::string escaped(const std::string& text) {
    std::string out;
    for (char c:text) {
        if (c=='\\') out+="\\\\";
        else if (c=='\n') out+="\\n";
        else if (c=='\r') out+="\\r";
        else if (c=='\t') out+="\\t";
        else out+=c;
    }
    return out;
}
std::string unescaped(const std::string& text) {
    std::string out;
    for (size_t i=0;i<text.size();++i) {
        char c=text[i];
        if (c=='\\') {
            if (++i==text.size()) throw Failure(SD_INVALID_ARGUMENT,"Incomplete setting escape");
            switch(text[i]) {
            case '\\':c='\\';break;case 'n':c='\n';break;case 'r':c='\r';break;case 't':c='\t';break;
            default:throw Failure(SD_INVALID_ARGUMENT,"Unknown setting escape");
            }
        }
        out+=c;
    }
    return out;
}
}
size_t SASettings::Count() { return std::size(schema); }
const SettingDefinition& SASettings::Definition(size_t i) {
    if (i>=Count()) throw Failure(SD_INVALID_ARGUMENT,"Unknown setting index");
    return schema[i];
}
size_t SASettings::Index(const char* key) {
    // Earlier SDK builds mislabelled APK menu minimization as extended keys.
    if(key && !std::strcmp(key,"ui.extended_keys")) key="ui.menu_state";
    if (key) for (size_t i=0;i<Count();++i) if (!std::strcmp(key,schema[i].key)) return i;
    throw Failure(SD_INVALID_ARGUMENT,"Unknown setting key");
}
SASettings::SASettings() {
    values_.reserve(Count());
    for(const auto& d:schema) {
        if(d.type==TEXT) values_.emplace_back(std::string(d.initial));
        else values_.emplace_back(number(d.initial));
    }
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw Failure(SD_INTERNAL_ERROR,"Cannot generate settings identity");
    wchar_t wide[40]{};StringFromGUID2(id,wide,40);
    std::string ascii;for(wchar_t c:wide) if(c) ascii+=char(c);
    Set("client.id",ascii);
}
int64_t SASettings::Integer(const char* key) const {
    const auto i=Index(key);
    if(schema[i].type==TEXT) throw Failure(SD_INVALID_ARGUMENT,"Setting requires text accessor");
    return std::get<int64_t>(values_[i]);
}
const std::string& SASettings::Text(const char* key) const {
    const auto i=Index(key);
    if(schema[i].type!=TEXT) throw Failure(SD_INVALID_ARGUMENT,"Setting requires integer accessor");
    return std::get<std::string>(values_[i]);
}
void SASettings::Set(const char* key,int64_t value) {
    const auto i=Index(key);const auto& d=schema[i];
    if(d.type==TEXT || value<d.minimum || value>d.maximum)
        throw Failure(SD_INVALID_ARGUMENT,"Setting type or range is invalid");
    if(*d.allowed) {
        const std::string allowed=std::string(",")+d.allowed+",";
        if(allowed.find(","+std::to_string(value)+",")==std::string::npos)
            throw Failure(SD_INVALID_ARGUMENT,"Unknown setting enum value");
    }
    values_[i]=value;
}
void SASettings::Set(const char* key,const std::string& value) {
    const auto i=Index(key);const auto& d=schema[i];
    if(d.type!=TEXT || value.size()<size_t(d.minimum) || value.size()>size_t(d.maximum) ||
       value.find('\0')!=std::string::npos || (!value.empty() &&
       !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0)))
        throw Failure(SD_INVALID_ARGUMENT,"Setting text length or UTF-8 is invalid");
    if (!std::strcmp(key,"client.id") || !std::strcmp(key,"client.name")) {
        const int units=value.empty()?0:MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),nullptr,0);
        if(units>(!std::strcmp(key,"client.id")?38:127))
            throw Failure(SD_INVALID_ARGUMENT,"Identity exceeds the UTF-16 wire field");
    }
    values_[i]=!std::strcmp(key,"client.id")?sd::CanonicalClientId(value):value;
}
std::vector<sd_resolution> SASettings::Resolutions() const {
    std::vector<sd_resolution> result;
    const auto& text=Text("display.resolutions");size_t start=0;
    while(start<text.size()) {
        const auto end=text.find(';',start),sep=text.find('x',start);
        const auto stop=end==std::string::npos?text.size():end;
        if(sep==std::string::npos || sep>=stop) throw Failure(SD_INVALID_ARGUMENT,"Use WIDTHxHEIGHT;WIDTHxHEIGHT resolutions");
        const auto w=number(text.substr(start,sep-start)),h=number(text.substr(sep+1,stop-sep-1));
        if(w<=0 || h<=0 || w>UINT32_MAX || h>UINT32_MAX || !sd::valid_size(uint32_t(w),uint32_t(h)))
            throw Failure(SD_INVALID_ARGUMENT,"Unsupported resolution dimensions");
        result.push_back({uint32_t(w),uint32_t(h)});
        if(result.size()>9) throw Failure(SD_INVALID_ARGUMENT,"At most nine resolutions are supported");
        if(end==std::string::npos) break;
        start=end+1;
        if(start==text.size()) throw Failure(SD_INVALID_ARGUMENT,"Empty resolution entry");
    }
    if(result.empty()) throw Failure(SD_INVALID_ARGUMENT,"At least one resolution is required");
    return result;
}
void SASettings::ValidateForClient() const {
    SASecureSocketLayerConfig::FromSettings(*this);
    if(Integer("display.bitrate_kbps"))
        throw Failure(SD_UNSUPPORTED,"display.bitrate_kbps: no direct bitrate command is established for this protocol profile; quality and FPS are supported");
    for (size_t i=0;i<Count();++i) if(schema[i].application&SD_SETTING_PENDING) {
        const auto& d=schema[i];
        const bool changed=d.type==TEXT?Text(d.key)!=d.initial:Integer(d.key)!=number(d.initial);
        if(changed) { const std::string message=std::string("Setting not implemented: ")+d.key;throw Failure(SD_UNSUPPORTED,message.c_str()); }
    }
    if(Integer("connection.transport")==2) throw Failure(SD_UNSUPPORTED,"Wi-Fi Direct is not implemented in the source APK");
    if(Integer("connection.transport")==1 && (Integer("security.encryption") || Integer("connection.websocket") ||
       Integer("client.type")!=0 || Integer("display.compression")!=3 || Integer("display.codec")==0))
        throw Failure(SD_UNSUPPORTED,"USB requires display type0, MJPEG base/codec3-5, and no TLS/WebSocket wrapper");
    if(Integer("client.type")>1) throw Failure(SD_UNSUPPORTED,"Reserved/audio client type is not a display session");
    if(Integer("connection.websocket") && Integer("client.type")!=1)
        throw Failure(SD_INVALID_ARGUMENT,"WebSocket display profile requires client.type=1");
    const auto capture=Integer("audio.capture_queue_ms");
    if(Integer("audio.enabled") && ((!Integer("audio.playback")&&!capture) || (capture&&capture<10)))
        throw Failure(SD_INVALID_ARGUMENT,"Enabled audio requires playback or a 10..10000 ms capture queue");
    Resolutions();
    MouseTarget();
    if(Integer("input.sonar_pen")) { sd_sonar_options audio{};sd_sonar_profile_options profiles{};SonarOptions(audio,profiles); }
}
void SASettings::SonarOptions(sd_sonar_options& audio,sd_sonar_profile_options& profiles) const {
    sd_sonar_options_init(&audio);sd_sonar_profile_options_init(&profiles);
    const auto copy_text=[&](const char* key,auto& target) {
        const auto& value=Text(key);if(value.size()>=sizeof(target)) throw Failure(SD_INVALID_ARGUMENT,"Sonar setting exceeds API buffer");
        std::memcpy(target,value.c_str(),value.size()+1);
    };
    copy_text("sonar.capture_endpoint_id",audio.capture_endpoint_id);copy_text("sonar.render_endpoint_id",audio.render_endpoint_id);
    if(!audio.capture_endpoint_id[0] || !audio.render_endpoint_id[0]) throw Failure(SD_INVALID_ARGUMENT,"Select both Sonar capture and render audio devices");
    const auto wide=[](const std::string& text) {
        const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
        if(!count) throw Failure(SD_INVALID_ARGUMENT,"Invalid UTF-8 Sonar endpoint ID");
        std::wstring result(size_t(count),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),count);return result;
    };
    const auto capture=wide(audio.capture_endpoint_id),render=wide(audio.render_endpoint_id);
    if(CompareStringOrdinal(capture.data(),int(capture.size()),render.data(),int(render.size()),TRUE)==CSTR_EQUAL)
        throw Failure(SD_INVALID_ARGUMENT,"Sonar capture and render endpoints must differ");
    audio.device_kind=uint32_t(Integer("sonar.device_kind"));audio.read_timeout_ms=uint32_t(Integer("sonar.read_timeout_ms"));
    copy_text("sonar.application_id",profiles.application_id);copy_text("sonar.store_path",profiles.store_path);
    copy_text("sonar.calibration_provider_path",profiles.calibration_provider_path);copy_text("sonar.compatibility_provider_path",profiles.compatibility_provider_path);
    for(const auto* path:{profiles.store_path,profiles.calibration_provider_path,profiles.compatibility_provider_path})
        if(*path && !std::filesystem::u8path(path).is_absolute()) throw Failure(SD_INVALID_ARGUMENT,"Sonar profile paths must be absolute");
}
sd_mouse_target SASettings::MouseTarget() const {
    sd_mouse_target target{};target.struct_size=sizeof(target);target.rotation=uint32_t(Integer("input.mouse_target_rotation"));
    const auto& desktop=Text("input.mouse_desktop_rect");const auto& display=Text("input.mouse_target_rect");
    if(desktop.empty()!=display.empty()) throw Failure(SD_INVALID_ARGUMENT,"Provide both remote mouse rectangles, or leave both empty for primary mirror");
    auto parse=[](const std::string& value,int32_t& left,int32_t& top,uint32_t& width,uint32_t& height) {
        int64_t values[4]{};size_t start=0;
        for(size_t i=0;i<4;++i) {
            const auto end=value.find(',',start);
            if((i<3)==(end==std::string::npos)) throw Failure(SD_INVALID_ARGUMENT,"Mouse rectangle format is left,top,width,height");
            values[i]=number(value.substr(start,end==std::string::npos?end:end-start));
            start=end==std::string::npos?value.size():end+1;
        }
        if(values[0]<INT32_MIN||values[0]>INT32_MAX||values[1]<INT32_MIN||values[1]>INT32_MAX||
           values[2]<=0||values[2]>INT32_MAX||values[3]<=0||values[3]>INT32_MAX)
            throw Failure(SD_INVALID_ARGUMENT,"Invalid mouse rectangle range");
        left=int32_t(values[0]);top=int32_t(values[1]);width=uint32_t(values[2]);height=uint32_t(values[3]);
    };
    if(!desktop.empty()) {
        parse(desktop,target.desktop_left,target.desktop_top,target.desktop_width,target.desktop_height);
        parse(display,target.display_left,target.display_top,target.display_width,target.display_height);
    }
    ValidateMouseTarget(target);return target;
}
std::string SASettings::Serialize(uint32_t flags) const {
    if(flags&~SD_SETTINGS_INCLUDE_SECRETS) throw Failure(SD_INVALID_ARGUMENT,"Unknown serialization flags");
    std::string out="SASettings\t1\n";
    for(const auto& d:schema) {
        if((d.application&SD_SETTING_SECRET) && !(flags&SD_SETTINGS_INCLUDE_SECRETS)) continue;
        out+=d.key;out+='\t';out+=d.type==TEXT?escaped(Text(d.key)):std::to_string(Integer(d.key));out+='\n';
    }
    return out;
}
void SASettings::Deserialize(const char* data,uint32_t bytes) {
    if(!data || bytes>1024*1024) throw Failure(SD_INVALID_ARGUMENT,"Invalid settings document size");
    const std::string text(data,bytes),prefix="SASettings\t1\n";
    if(text.compare(0,prefix.size(),prefix) || text.find('\0')!=std::string::npos)
        throw Failure(SD_INVALID_ARGUMENT,"Unsupported settings document/version");
    SASettings candidate=*this;std::set<size_t> seen;size_t pos=prefix.size();
    while(pos<text.size()) {
        const auto end=text.find('\n',pos),tab=text.find('\t',pos);
        if(end==std::string::npos || tab==std::string::npos || tab>=end)
            throw Failure(SD_INVALID_ARGUMENT,"Malformed settings record");
        const auto key=text.substr(pos,tab-pos),value=text.substr(tab+1,end-tab-1);
        const auto index=Index(key.c_str());
        if(!seen.insert(index).second) throw Failure(SD_INVALID_ARGUMENT,"Duplicate settings key or alias");
        if(schema[index].type==TEXT) candidate.Set(key.c_str(),unescaped(value));
        else candidate.Set(key.c_str(),number(value));
        pos=end+1;
    }
    Swap(candidate);
}
}

namespace {
template<class F> sd_result access(const sd_settings* s,F&& f) noexcept {
    if(!s) return SD_INVALID_ARGUMENT;
    if(s->owner!=GetCurrentThreadId()) return SD_WRONG_THREAD;
    try { f();return SD_OK; }
    catch(const sd::Failure& e) { return e.status; }
    catch(const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
    catch(...) { return SD_INTERNAL_ERROR; }
}
void copy(const std::string& value,char* out,uint32_t capacity,uint32_t* required) {
    if(!required || (!out&&capacity)) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid output buffer");
    *required=uint32_t(value.size()+1);
    if(!out && !capacity) return;
    if(capacity<*required) throw sd::Failure(SD_INVALID_ARGUMENT,"Output buffer is too small");
    std::memcpy(out,value.c_str(),*required);
}
}
extern "C" {
sd_result SD_CALL sd_settings_create(sd_settings** out) {
    if(!out) return SD_INVALID_ARGUMENT;*out=nullptr;
    try { *out=new sd_settings;return SD_OK; }
    catch(const sd::Failure& e) { return e.status; }
    catch(const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
    catch(...) { return SD_INTERNAL_ERROR; }
}
sd_result SD_CALL sd_settings_destroy(sd_settings* s) {
    if(!s) return SD_OK;
    if(s->owner!=GetCurrentThreadId()) return SD_WRONG_THREAD;
    delete s;return SD_OK;
}
uint32_t SD_CALL sd_settings_count() { return uint32_t(sd::native::SASettings::Count()); }
sd_result SD_CALL sd_settings_describe(uint32_t i,sd_setting_info* out) {
    if(!out || out->struct_size!=sizeof(*out) || i>=sd_settings_count()) return SD_INVALID_ARGUMENT;
    const auto& d=sd::native::SASettings::Definition(i);
    *out={sizeof(*out),d.type,d.application,0,d.minimum,d.maximum,d.key,d.initial,d.allowed,d.apk};return SD_OK;
}
sd_result SD_CALL sd_settings_get_integer(const sd_settings* s,const char* key,int64_t* out) {
    if(!out) return SD_INVALID_ARGUMENT;return access(s,[&]{*out=s->value.Integer(key);});
}
sd_result SD_CALL sd_settings_set_integer(sd_settings* s,const char* key,int64_t value) {
    return access(s,[&]{s->value.Set(key,value);});
}
sd_result SD_CALL sd_settings_get_text(const sd_settings* s,const char* key,char* out,uint32_t capacity,uint32_t* required) {
    return access(s,[&]{copy(s->value.Text(key),out,capacity,required);});
}
sd_result SD_CALL sd_settings_set_text(sd_settings* s,const char* key,const char* value) {
    if(!value) return SD_INVALID_ARGUMENT;return access(s,[&]{s->value.Set(key,std::string(value));});
}
sd_result SD_CALL sd_settings_serialize(const sd_settings* s,uint32_t flags,char* out,uint32_t capacity,uint32_t* required) {
    return access(s,[&]{copy(s->value.Serialize(flags),out,capacity,required);});
}
sd_result SD_CALL sd_settings_deserialize(sd_settings* s,const char* data,uint32_t bytes) {
    return access(s,[&]{s->value.Deserialize(data,bytes);});
}
}
