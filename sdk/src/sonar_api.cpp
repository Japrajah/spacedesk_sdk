#include "sonar_session.h"
#include "motion_input.h"
#include "settings.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

struct sd_sonar_session {
    DWORD owner=GetCurrentThreadId();
    std::unique_ptr<sd::native::SonarSessionRuntime> runtime;
    std::string error_view;
};
namespace {
std::wstring wide(const char* value,size_t count) {
    if(!count) return {};
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,int(count),nullptr,0);
    if(!size) throw sd::Failure(SD_INVALID_ARGUMENT,"Sonar text must be valid UTF-8");
    std::wstring result(size,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,int(count),result.data(),size);return result;
}
sd_result owner(sd_sonar_session* session) { return !session?SD_INVALID_ARGUMENT:session->owner!=GetCurrentThreadId()?SD_WRONG_THREAD:SD_OK; }
template<class F> sd_result call(sd_sonar_session* session,F&& fn) {
    const auto valid=owner(session);if(valid!=SD_OK) return valid;
    try { const auto result=fn();session->error_view=result<0?session->runtime->LastError():std::string();return result; }
    catch(const sd::Failure& error) { session->error_view=error.what();return error.status; }
    catch(const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
    catch(const std::exception& error) { session->error_view=error.what();return SD_INTERNAL_ERROR; }
    catch(...) { return SD_INTERNAL_ERROR; }
}
}
extern "C" {
sd_result SD_CALL sd_sonar_options_init(sd_sonar_options* options) {
    if(!options) return SD_INVALID_ARGUMENT;*options={};options->struct_size=sizeof(*options);options->read_timeout_ms=250;return SD_OK;
}
sd_result SD_CALL sd_sonar_create(const sd_sonar_options* options,sd_sonar_session** output) {
    return sd_sonar_create_with_profiles(options,nullptr,output);
}
sd_result SD_CALL sd_sonar_profile_options_init(sd_sonar_profile_options* options) {
    if(!options) return SD_INVALID_ARGUMENT;*options={};options->struct_size=sizeof(*options);
    std::memcpy(options->application_id,"spacedesk.sdk",14);return SD_OK;
}
sd_result SD_CALL sd_sonar_create_with_profiles(const sd_sonar_options* options,const sd_sonar_profile_options* profiles,sd_sonar_session** output) {
    if(!output) return SD_INVALID_ARGUMENT;*output=nullptr;
    if(!options || options->struct_size!=sizeof(*options) || options->device_kind>SD_SONAR_DEVICE_SP1 || options->reserved ||
       options->read_timeout_ms<10 || options->read_timeout_ms>5000 ||
       !std::memchr(options->capture_endpoint_id,0,sizeof(options->capture_endpoint_id)) ||
       !std::memchr(options->render_endpoint_id,0,sizeof(options->render_endpoint_id)) ||
       !options->capture_endpoint_id[0] || !options->render_endpoint_id[0]) return SD_INVALID_ARGUMENT;
    try {
        sd::native::SonarSessionConfig config;
        if(profiles) {
            if(profiles->struct_size!=sizeof(*profiles) || profiles->reserved || !profiles->application_id[0] ||
               !std::memchr(profiles->application_id,0,sizeof(profiles->application_id))) return SD_INVALID_ARGUMENT;
            config.profiles.application=profiles->application_id;wide(config.profiles.application.data(),config.profiles.application.size());
            const auto path=[&](const char* bytes,size_t capacity) {
                if(!std::memchr(bytes,0,capacity)) throw sd::Failure(SD_INVALID_ARGUMENT,"Unterminated Sonar profile path");
                auto value=wide(bytes,std::strlen(bytes));
                if(!value.empty() && !std::filesystem::path(value).is_absolute()) throw sd::Failure(SD_INVALID_ARGUMENT,"Sonar profile paths must be absolute");
                return value;
            };
            config.profiles.store=path(profiles->store_path,sizeof(profiles->store_path));
            config.profiles.calibration=path(profiles->calibration_provider_path,sizeof(profiles->calibration_provider_path));
            config.profiles.compatibility=path(profiles->compatibility_provider_path,sizeof(profiles->compatibility_provider_path));
        }
        config.input=wide(options->capture_endpoint_id,std::strlen(options->capture_endpoint_id));
        config.output=wide(options->render_endpoint_id,std::strlen(options->render_endpoint_id));
        if(CompareStringOrdinal(config.input.c_str(),int(config.input.size()),config.output.c_str(),int(config.output.size()),TRUE)==CSTR_EQUAL) return SD_INVALID_ARGUMENT;
        config.kind=options->device_kind;config.timeout=options->read_timeout_ms;
        auto session=std::make_unique<sd_sonar_session>();session->runtime=std::make_unique<sd::native::SonarSessionRuntime>(std::move(config));
        *output=session.release();return SD_OK;
    } catch(const sd::Failure& error) { return error.status; }
      catch(const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
      catch(...) { return SD_INTERNAL_ERROR; }
}
sd_result SD_CALL sd_sonar_start(sd_sonar_session* session) { return call(session,[&]{return session->runtime->Start();}); }
sd_result SD_CALL sd_sonar_create_from_settings(const sd_settings* settings,sd_sonar_session** output) {
    if(!output) return SD_INVALID_ARGUMENT;*output=nullptr;
    if(!settings) return SD_INVALID_ARGUMENT;
    if(settings->owner!=GetCurrentThreadId()) return SD_WRONG_THREAD;
    try {
        if(!settings->value.Integer("input.sonar_pen")) return SD_IGNORED;
        sd_sonar_options audio{};sd_sonar_profile_options profiles{};settings->value.SonarOptions(audio,profiles);
        const auto created=sd_sonar_create_with_profiles(&audio,&profiles,output);if(created!=SD_OK) return created;
        const auto palm=sd_sonar_set_palm_rejection(*output,uint32_t(settings->value.Integer("input.sonar_palm_rejection")));
        if(palm!=SD_OK) { sd_sonar_destroy(*output);*output=nullptr; }return palm;
    } catch(const sd::Failure& error) { return error.status; }
      catch(const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
      catch(...) { return SD_INTERNAL_ERROR; }
}
sd_result SD_CALL sd_sonar_stop(sd_sonar_session* session) { return call(session,[&]{return session->runtime->Stop();}); }
sd_result SD_CALL sd_sonar_destroy(sd_sonar_session* session) {
    const auto valid=owner(session);if(valid!=SD_OK) return valid;delete session;return SD_OK;
}
sd_result SD_CALL sd_sonar_get_state(sd_sonar_session* session,sd_sonar_state* state) {
    return call(session,[&]{if(!state || state->struct_size!=sizeof(*state)) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar state structure");session->runtime->State(*state);return sd_result(SD_OK);});
}
sd_result SD_CALL sd_sonar_get_pointer_state(sd_sonar_session* session,sd_sonar_pointer_state* state) {
    return call(session,[&] { if(!state || state->struct_size!=sizeof(*state)) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar pointer state");
        session->runtime->PointerState(*state);return SD_OK; });
}
sd_result SD_CALL sd_sonar_poll(sd_sonar_session* session,uint32_t timeout,sd_sonar_event* event) {
    return call(session,[&]{if(!event || event->struct_size!=sizeof(*event) || timeout>60000) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar event structure or timeout");return session->runtime->Poll(timeout,*event);});
}
sd_result SD_CALL sd_sonar_set_profile(sd_sonar_session* session,const char* text,uint32_t size) {
    return call(session,[&]{if((!text && size) || size>65536) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar profile buffer");wide(text,size);return session->runtime->Profile(size?std::string(text,size):std::string());});
}
sd_result SD_CALL sd_sonar_profile_receive(sd_sonar_session* session,const char* to,const char* sender,const char* text,uint32_t size) {
    return call(session,[&] {
        if(!to || !sender || (!text&&size) || size>65536 || strnlen_s(to,1025)>1024 || strnlen_s(sender,1025)>1024)
            throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar profile message");
        wide(to,std::strlen(to));wide(sender,std::strlen(sender));wide(text,size);
        return session->runtime->ProfileReceive(to,sender,size?std::string(text,size):std::string());
    });
}
sd_result SD_CALL sd_sonar_get_profile(sd_sonar_session* session,sd_sonar_profile_state* state,char* profile,uint32_t capacity) {
    return call(session,[&] {
        if(!state || state->struct_size!=sizeof(*state) || (!profile&&capacity)) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar profile output");
        sd::native::SonarPreferenceSnapshot snapshot;
        const auto result=session->runtime->ProfileRead(snapshot);if(result!=SD_OK) return result;
        if(profile && capacity<snapshot.profile.size()+1) throw sd::Failure(SD_INVALID_ARGUMENT,"Sonar profile output buffer is too small");
        sd_sonar_profile_state value{sizeof(value),snapshot.flags,uint32_t(snapshot.profile.size()),snapshot.result};
        if(profile) std::memcpy(profile,snapshot.profile.c_str(),snapshot.profile.size()+1);
        *state=value;return sd_result(SD_OK);
    });
}
sd_result SD_CALL sd_sonar_refresh_devices(sd_sonar_session* session) { return call(session,[&]{return session->runtime->Refresh();}); }
sd_result SD_CALL sd_sonar_key(sd_sonar_session* session,uint32_t code,uint32_t action) {
    return call(session,[&]{if(action>2) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar key action");return session->runtime->Key(code,action);});
}
sd_result SD_CALL sd_sonar_set_palm_rejection(sd_sonar_session* session,uint32_t enabled) {
    return call(session,[&]{if(enabled>1) throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid palm-rejection flag");return session->runtime->Palm(enabled!=0);});
}
sd_result SD_CALL sd_sonar_process_motion(sd_sonar_session* session,const sd_sonar_motion* input,sd_sonar_motion_result* output) {
    return call(session,[&] {
        if(!sd::ValidMotionEvent(input) || !output || output->struct_size!=sizeof(*output))
            throw sd::Failure(SD_INVALID_ARGUMENT,"Invalid Sonar motion structure or pointer index");
        sd::native::SonarMotionEvent event;
        event.down_time=input->down_time_ms;event.event_time=input->event_time_ms;event.action=int32_t(input->action);
        event.meta_state=int32_t(input->meta_state);event.buttons=int32_t(input->button_state);event.device=input->device_id;
        event.edge_flags=int32_t(input->edge_flags);event.source=int32_t(input->source);event.flags=int32_t(input->flags);
        event.x_precision=input->x_precision;event.y_precision=input->y_precision;
        for(uint32_t i=0;i<input->pointer_count;++i) {
            const auto& point=input->pointers[i];
            sd::native::SonarMotionPoint copy;copy.id=point.pointer_id;copy.tool=point.tool_type;
            std::copy(std::begin(point.axes),std::end(point.axes),copy.axes.begin());event.points.push_back(copy);
        }
        sd::native::SonarMotionResult transformed;int32_t action_after=0;
        const auto result=session->runtime->Motion(std::move(event),transformed,action_after);if(result!=SD_OK) return result;
        sd_sonar_motion_result value{};value.struct_size=sizeof(value);value.rebuilt=transformed.rebuilt;value.input_action_after=uint32_t(action_after);
        const auto& from=transformed.event;auto& to=value.event;to.struct_size=sizeof(to);to.action=uint32_t(from.action);to.pointer_count=uint32_t(from.points.size());
        to.meta_state=uint32_t(from.meta_state);to.button_state=uint32_t(from.buttons);to.device_id=from.device;to.edge_flags=uint32_t(from.edge_flags);
        to.source=uint32_t(from.source);to.flags=uint32_t(from.flags);to.down_time_ms=from.down_time;to.event_time_ms=from.event_time;
        to.x_precision=from.x_precision;to.y_precision=from.y_precision;
        if(from.points.size()>SD_SONAR_MAX_POINTERS) throw sd::Failure(SD_INTERNAL_ERROR,"Sonar output exceeds pointer capacity");
        for(size_t i=0;i<from.points.size();++i) {
            to.pointers[i].pointer_id=from.points[i].id;to.pointers[i].tool_type=from.points[i].tool;
            std::copy(from.points[i].axes.begin(),from.points[i].axes.end(),std::begin(to.pointers[i].axes));
        }
        *output=value;return sd_result(SD_OK);
    });
}
const char* SD_CALL sd_sonar_last_error(sd_sonar_session* session) {
    if(!session) return "Null Sonar session";if(owner(session)!=SD_OK) return "Wrong Sonar session owner thread";
    try { if(session->error_view.empty()) session->error_view=session->runtime->LastError();return session->error_view.c_str(); }
    catch(...) { return "Cannot retrieve Sonar error"; }
}
}
