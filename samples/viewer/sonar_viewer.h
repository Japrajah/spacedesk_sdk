#pragma once
#include "spacedesk_settings.hpp"
#include "sonar_touch.h"
#include <functional>
#include <memory>
#include <string>

namespace sd::viewer {
struct SonarViewerApi {
    decltype(&sd_sonar_create_from_settings) create=sd_sonar_create_from_settings;
    decltype(&sd_sonar_start) start=sd_sonar_start;
    decltype(&sd_sonar_stop) stop=sd_sonar_stop;
    decltype(&sd_sonar_destroy) destroy=sd_sonar_destroy;
    decltype(&sd_sonar_get_state) state=sd_sonar_get_state;
    decltype(&sd_sonar_get_pointer_state) pointer=sd_sonar_get_pointer_state;
    decltype(&sd_sonar_poll) poll=sd_sonar_poll;
    decltype(&sd_sonar_process_motion) motion=sd_sonar_process_motion;
    decltype(&sd_sonar_key) key=sd_sonar_key;
    decltype(&sd_sonar_last_error) error=sd_sonar_last_error;
    decltype(&sd_client_send_mapped_motion) send=sd_client_send_mapped_motion;
    decltype(&sd_client_send_pen_button) button=sd_client_send_pen_button;
};
// Windows frontend owner for n2 + SAActivityDisplay view dispatch. It contains
// no packet serialization, audio loop or Sonar classification implementation.
class SonarViewer {
    SonarViewerApi api_;
    sd_client* client_=nullptr;sd_sonar_session* session_=nullptr;
    std::unique_ptr<SASettings> settings_;
    SonarTouchBatch batch_;
    bool attempted_=false,started_=false,key_down_=false,media_down_=false,release_media_=false;
    sd_sonar_state state_{sizeof(sd_sonar_state)};
    struct Sent { sd_motion_surface surface;sd_motion_event event; };
    std::optional<Sent> pen_,touch_;
    std::string error_;
    void Error(sd_result result) {
        if(result<0) { const char* text=session_?api_.error(session_):nullptr;error_=text&&*text?text:"Cannot initialize SonarPen"; }
    }
    void ReleaseMedia() {
        release_media_=release_media_||media_down_;
        if(!release_media_ || !session_) return;
        const auto released=api_.key(session_,85,1);
        if(released==SD_OK) media_down_=release_media_=false;else Error(released);
    }
    sd_result ReleaseNetwork(bool release_key=true) {
        if(release_key) ReleaseMedia();
        sd_result result=SD_IGNORED;
        if(pen_) {
            auto last=*pen_;pen_.reset();last.event.action=1;
            for(uint32_t i=0;i<last.event.pointer_count;++i) last.event.pointers[i].axes[2]=0;
            const auto sent=api_.send(client_,SD_MOTION_OUTPUT_PEN,&last.surface,&last.event);if(sent<0 || result>=0) result=sent;
        }
        if(touch_) {
            auto last=*touch_;touch_.reset();last.event.action=3;
            const auto sent=api_.send(client_,SD_MOTION_OUTPUT_TOUCH,&last.surface,&last.event);if(sent<0 || result>=0) result=sent;
        }
        if(release_key && key_down_) { key_down_=false;const auto sent=api_.button(client_,1);if(sent<0 || result>=0) result=sent; }
        return result;
    }
public:
    explicit SonarViewer(SonarViewerApi api={}):api_(api) {}
    ~SonarViewer() { try { Close(); } catch(...) { if(session_) api_.destroy(session_); } }
    SonarViewer(const SonarViewer&)=delete;
    SonarViewer& operator=(const SonarViewer&)=delete;
    bool Enabled() const noexcept { return bool(settings_); }
    bool Available() const noexcept { return session_ && started_; }
    bool Tracked(uint32_t id) const noexcept { return batch_.Tracked(id); }
    uint32_t MaximumContactDiameter() const { return settings_?uint32_t(settings_->GetInteger("sonar.contact_size_max_px")):0; }
    const std::string& LastError() const noexcept { return error_; }
    void Configure(const SASettings& settings,sd_client* client) {
        Close();client_=client;error_.clear();settings_.reset();
        if(!settings.GetInteger("input.sonar_pen")) return;
        auto next=std::make_unique<SASettings>();next->Deserialize(settings.Serialize(SD_SETTINGS_INCLUDE_SECRETS));settings_=std::move(next);
    }
    void Activate() {
        if(!settings_ || attempted_) return;attempted_=true;error_.clear();
        if(!session_) { const auto made=api_.create(settings_->Native(),&session_);if(made!=SD_OK) { Error(made);return; } }
        const auto result=api_.start(session_);started_=result==SD_OK;Error(result);Poll();
    }
    sd_result ResetInput() {
        if(auto release=batch_.Release(int64_t(GetTickCount64()));release && session_) {
            sd_sonar_motion_result ignored{sizeof(ignored)};Error(api_.motion(session_,&*release,&ignored));
        }
        return ReleaseNetwork();
    }
    void Deactivate() {
        ResetInput();if(session_ && started_) Error(api_.stop(session_));started_=attempted_=false;
        media_down_=release_media_=false;
        state_={sizeof(state_)};
    }
    void Close() {
        Deactivate();if(session_) { api_.destroy(session_);session_=nullptr; }
    }
    void Poll() {
        if(!session_) return;
        for(unsigned i=0;i<16;++i) {
            sd_sonar_event event{sizeof(event)};const auto result=api_.poll(session_,0,&event);
            if(result!=SD_OK) break;if(event.result<0) Error(event.result);
        }
        Error(api_.state(session_,&state_));
        if(release_media_ && state_.core_ready) ReleaseMedia();
    }
    std::wstring Status() const {
        if(!settings_) return {};
        if(!error_.empty() && (!session_ || !state_.running)) return L"Sonar: unavailable";
        if(!started_) return L"Sonar: paused";
        if(!state_.device_connected) return L"Sonar: waiting for device";
        if(state_.raw_status<0 || state_.raw_status==7 || state_.raw_status==8) return L"Sonar: signal not detected";
        if(state_.calibrating) return L"Sonar: calibrating";
        return L"Sonar: ready";
    }
    // Map owns window/DPI/renderer adaptation. The DLL keeps W1/V1 fallback and
    // routing predicates copied from the APK stay here with the view policy.
    template<class Map> sd_result Touch(UINT message,const SonarTouchSample& sample,bool pen_enabled,bool touch_enabled,
        uint64_t wallclock_ms,uint32_t width,uint32_t height,Map&& map) {
        if(!Available()) return SD_IGNORED;
        if(sample.canceled || message==WM_POINTERCAPTURECHANGED || message==WM_POINTERLEAVE ||
           (message==WM_POINTERUPDATE && !sample.contact)) return Tracked(sample.windows_id)?ResetInput():SD_IGNORED;
        const auto input=batch_.Process(message,sample,int64_t(GetTickCount64()));if(!input) return SD_IGNORED;
        sd_sonar_motion_result result{sizeof(result)};const auto processed=api_.motion(session_,&*input,&result);
        if(processed!=SD_OK) { Error(processed);const auto released=ResetInput();return released<0?released:SD_IGNORED; }
        sd_sonar_pointer_state pointer{sizeof(pointer),0,-1,-1};Error(api_.pointer(session_,&pointer));
        auto& event=result.event;
        const bool pen=pen_enabled && (pointer.pen_active || (event.pointer_count && event.pointers[0].tool_type==2));
        sd_motion_surface surface{sizeof(surface),width,height,0,uint32_t(wallclock_ms)};
        for(uint32_t i=0;i<event.pointer_count;++i) if(map(event.pointers[i])) surface.mapped_mask|=1u<<i;
        sd_result sent=SD_IGNORED;
        if(event.pointer_count && (pen || touch_enabled)) {
            sent=api_.send(client_,pen?SD_MOTION_OUTPUT_PEN:SD_MOTION_OUTPUT_TOUCH,&surface,&event);
            if(sent==SD_OK) {
                auto& retained=pen?pen_:touch_;
                if(event.action==1 || (!pen && event.action==3)) retained.reset();else retained=Sent{surface,event};
            }
        }
        // Win32 gesture termination must also release a route used earlier in
        // the gesture (e.g. a secondary selected Sonar contact). Source packet
        // builders remain unchanged; this is frontend lifetime cleanup.
        if(!batch_.Count()) { const auto released=ReleaseNetwork(false);if(released<0) sent=released; }
        return sent;
    }
    bool MediaKey(bool up,sd_result* output=nullptr) {
        if(output) *output=SD_IGNORED;
        if(!Available()) return false;
        const auto accepted=api_.key(session_,85,up?1u:0u);
        if(accepted!=SD_OK && !(up&&(key_down_||media_down_))) { Error(accepted);return false; }
        if(accepted==SD_OK) { media_down_=!up;release_media_=false; }
        else if(up && media_down_) release_media_=true;
        const auto sent=accepted==SD_OK || key_down_?api_.button(client_,up?1u:0u):SD_IGNORED;if(output) *output=sent;
        if(up) key_down_=false;else if(sent==SD_OK) key_down_=true;
        return true;
    }
};
}
