#include "sonar_controller.h"
#include "sonar_signal.h"
#include "sonar_profile.h"
#include "sonar_profile_exchange.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sd::native {
namespace {
int32_t JavaRound(float value) {
    if(std::isnan(value)) return 0;
    if(double(value)>=double(INT32_MAX)) return INT32_MAX;
    if(double(value)<=double(INT32_MIN)) return INT32_MIN;
    return int32_t(std::floor(double(value)+0.5));
}
}
SonarPenController::SonarPenController(SonarPlatform& platform):platform_(platform) {
    if(auto environment=platform_.ProfileEnvironment()) {
        SonarProfileExchange exchange(*environment,[this](const std::string& profile){ApplyProfile(profile);});
        exchange.Bootstrap();
    }
}
bool SonarPenController::ReceiveProfile(const std::string& to,const std::string& sender,const std::string& text) {
    auto environment=platform_.ProfileEnvironment();if(!environment) return false;
    return SonarProfileExchange(*environment,[this](const std::string& profile){ApplyProfile(profile);}).Receive(to,sender,text);
}
bool SonarPlatform::Read(bool calibration,SonarPenState& q) {
    if(!HasRecorder()) {
        try { CreateRecorder(SonarRecordFormat(MinimumRecordBytes())); }
        catch(...) { ForgetRecorder();q.m0=1; }
        if(HasRecorder() && RecordingState()!=3) {
            try { StartRecorder(); } catch(...) { q.m0=2; }
        }
    }
    if(calibration) {
        std::vector<uint8_t> bytes(8192);
        const int32_t count=ReadBytes(bytes.data(),uint32_t(bytes.size()));
        if(count<4) return false;
        // Original arraycopy/W0.a.d throw on oversized or odd byte counts.
        if(size_t(count)>bytes.size() || count%2) throw std::runtime_error("Invalid SonarPen PCM byte count");
        std::vector<int16_t> samples(size_t(count)/2);
        int32_t maximum=0,minimum=0;
        for(size_t i=0;i<samples.size();++i) {
            samples[i]=int16_t(uint16_t(bytes[i*2])|(uint16_t(bytes[i*2+1])<<8));
            maximum=std::max(maximum,int32_t(samples[i]));minimum=std::min(minimum,int32_t(samples[i]));
        }
        q.a0=float(std::max(maximum,-minimum)); // c() precedes potentially failing FFT i().
        SonarSignalAnalyzer analyzer;analyzer.Reset(samples.data(),uint32_t(samples.size()));
        const auto signal=analyzer.Analyze();q.b0=float(signal.frequency_hz);q.c0=float(signal.level);
        return true;
    }
    try {
        std::vector<int16_t> samples(4096);
        const int32_t count=ReadSamples(samples.data(),uint32_t(samples.size()));
        if(count>0) {
            if(size_t(count)>samples.size()) throw std::runtime_error("Invalid SonarPen PCM sample count");
            const float amplitude=SonarSteadyAmplitude(samples.data(),uint32_t(count));
            if(amplitude>0) q.a0=amplitude;
        } else q.m0=count;
        return true; // Nonpositive reads keep the preceding amplitude in the APK.
    } catch(...) { q.m0=4;return false; }
}
void SonarPenController::Status(int32_t value) {
    if(state_.R!=value && state_.C) platform_.Status(value);
    state_.R=value;
}
void SonarPenController::ConfigureVolume() {
    if(!platform_.HasAudioService()) return;
    int32_t desired=platform_.MaximumVolume();
    if(state_.q0) desired=JavaRound((float(desired)*state_.r0)/100.0f);
    else if(state_.o0 && state_.G0) desired=JavaRound((float(desired)*state_.p0)/100.0f);
    try { platform_.SetVolume(desired); } catch(...) {}
    if(platform_.CurrentVolume()<desired) { platform_.AdjustVolumeUp();Status(-3); }
}
void SonarPenController::RestoreVolume() {
    if(!platform_.HasAudioService()) return;
    try { platform_.SetVolume(state_.B); } catch(...) {}
}
void SonarPenController::CreateTone(bool right) {
    if(platform_.HasTrack()) {
        if(platform_.TrackPlaying()) platform_.StopTrack();
        platform_.ReleaseTrack();
    }
    std::vector<int16_t> samples(88200);
    SonarTone(state_.h,uint32_t(right),samples.data());
    platform_.CreateTrack(SonarTrackFormat{});
    if(platform_.TrackState()!=0) {
        platform_.WriteTrack(samples.data(),uint32_t(samples.size()));
        if(platform_.TrackState()==1) {
            // Original n ignores short writes and negative return codes here;
            // only the track state after write selects its cleanup branch.
            platform_.SetTrackLoop(0,22050,-1);
            platform_.SetTrackGain(platform_.MaximumTrackGain());
            return;
        }
    }
    platform_.ReleaseTrack();
}
void SonarPenController::StopPlayingTrack() {
    if(platform_.HasTrack() && platform_.TrackPlaying()) {
        platform_.StopTrack();platform_.ReleaseTrack();state_.l=0;
    }
}
void SonarPenController::StopRecording() {
    if(platform_.HasRecorder()) {
        try { platform_.StopRecorder();platform_.ReleaseRecorder(); } catch(...) {}
        platform_.ForgetRecorder();
    }
    state_.V=0;
}
void SonarPenController::CalibrationFailure() {
    StopPlayingTrack();StopRecording();state_.A=false;Status(7);
    state_.l=0;state_.m=0;state_.u=false;state_.X=0;
}
float SonarPenController::Threshold(float amplitude) const {
    const auto& q=state_;
    const float ratio=q.h0/q.f0;
    if(q.G0 && q.s) return q.T*(double(ratio)<2.25?1.12f:1.25f);
    if(q.f0!=0 && double(ratio)>=2.25 && ratio<=15 && q.g0<q.h0) {
        const float divisor=(q.h0-q.f0)/q.h0;
        if(q.s && q.g0>0) return q.g0/divisor;
        return amplitude/divisor;
    }
    return amplitude*(double(ratio)<2.25?1.12f:1.25f);
}
void SonarPenController::Calibrate() noexcept {
    auto& q=state_;
    try {
        if(q.l==0 && platform_.HasTrack()) {
            try { platform_.StopTrack(); } catch(...) {}
            platform_.ReleaseTrack();
        }
        if(!q.A) return;
        if(q.s || q.t) q.h=1000;
        if(!platform_.Read(true,q)) return;
        if(q.l==0) {
            ConfigureVolume();Status(1);q.d0=0;q.e0=0;q.V=0;++q.l;return;
        }
        if(q.l==20 || q.l==60) {
            const bool right=q.l==60;
            if(!right) ConfigureVolume();
            (right?q.f0:q.i0)=q.e0>0?q.d0/float(q.e0):0;
            q.d0=0;q.e0=0;CreateTone(right);
            if(!platform_.HasTrack()) { Status(-2);q.l=0;StopRecording();q.A=false;return; }
            platform_.PlayTrack();++q.l;return;
        }
        if(q.l==40) {
            q.g0=q.e0>0?q.d0/float(q.e0):0;
            if(!platform_.HasTrack()) { CalibrationFailure();return; }
            platform_.StopTrack();platform_.ReleaseTrack();++q.l;q.d0=0;q.e0=0;return;
        }
        if(q.l==80) {
            if(q.e0<=0) {
                if(q.h==9000) {
                    q.h=250;platform_.StopTrack();platform_.ReleaseTrack();q.l=0;q.d0=0;q.e0=0;
                    platform_.SaveLowFrequency();return;
                }
                CalibrationFailure();return;
            }
            q.h0=q.d0/float(q.e0);
            platform_.SaveReadings(q.f0,q.g0,q.h0,q.i0,q.b0);
            if(!(q.b0>q.h-1 && q.b0<q.h+1) && !q.t0 && !q.s && !q.t) {
                CalibrationFailure();return;
            }
            bool detected;
            if(q.f0>0 && !platform_.HonorOrHuawei()) {
                const float right=q.h0/q.f0,left=q.g0/q.f0,noise=q.i0/q.f0;
                const float ratio=right/left;
                detected=left>0 && left>0.885f && right>noise && ratio>1 && ratio<7;
            } else detected=double(q.h0)>double(q.g0)*1.3;
            if(!detected && !q.t0) { CalibrationFailure();return; }
            ConfigureVolume();q.A=false;q.r=true;Status(detected?6:8);
            q.w=q.s && q.f0>0?q.g0*(q.g0/q.f0):(q.h0*6)/10;
            q.z=40*q.h0;
            q.y=(q.s && q.i0>0?q.h0/q.i0:5)*q.h0;
            q.V=Threshold(q.h0);q.W=true;
            if(!q.s && q.f0<200) q.W=false;
            q.n=-1;q.m=0;q.u=false;q.X=0;return;
        }
        if(q.l<80) {
            const bool frequency=q.b0>q.h-1 && q.b0<q.h+1;
            if((q.l>=20 || q.c0<-1) && (frequency || q.s || q.t)) { q.d0+=q.a0;++q.e0; }
            ++q.l;
        }
    } catch(...) {} // The original g0 catches platform exceptions with partial state retained.
}
void SonarPenController::UpdatePressure() noexcept {
    auto& q=state_;
    try {
        if(!q.Z || !q.r || !q.H || q.B0) return;
        const float amplitude=q.a0;
        if(q.D0) {
            if(amplitude<2) {
                if(!q.u) { q.u=true;if(q.C) platform_.PostButton(); }
            } else q.u=false;
        }
        if(q.u || !(amplitude<q.z)) return;
        if(q.y<amplitude) q.y=amplitude;
        if(amplitude<q.f0) {
            if(++q.j0>10) {
                q.j0=0;try { platform_.StopRecorder(); } catch(...) {}
                platform_.ForgetRecorder();
            }
        } else q.j0=0;
        if(!q.r || !q.v) { Status(2);q.D=-1;q.m=0;return; }
        if(amplitude>=q.V || q.D!=-1) {
            Status(3);q.p=amplitude;
            if(q.S || (q.G0 && q.U>0)) q.m=SonarPressureCurve(q.U0,amplitude,q.V!=0?q.V:q.T,q.U);
            else if(q.V==amplitude || q.V==0) q.m=0.011f;
            else q.m=SonarPressureCurve(q.U0,amplitude,std::max(q.w,q.V),std::min(q.y,q.z*3));
        }
        q.l=100;
    } catch(...) {}
}
int32_t SonarPenController::Tick() {
    auto& q=state_;
    if(!q.k) { platform_.ForgetWorker();return -1; }
    if(q.B0) return 100;
    if(!q.H) {
        Status(5);StopRecording();StopPlayingTrack();q.l=0;q.m=0;q.u=false;q.r=false;q.A=true;q.I=20;
        return 50;
    }
    if(q.A) Calibrate();
    else {
        q.Z=platform_.Read(false,q);
        if(q.Z) UpdatePressure();
    }
    return 0;
}
void SonarPenController::RegisterHeadset() {
    if(state_.l0) return;
    try { platform_.RegisterHeadset(); } catch(...) {}
    state_.l0=true; // Original marks registration even if registerReceiver threw.
}
void SonarPenController::UnregisterHeadset() {
    if(!state_.l0) return;
    try { platform_.UnregisterHeadset(); } catch(...) {}
    state_.l0=false;
}
void SonarPenController::StopProfileWatcher() {
    if(platform_.HasProfileWatcher()) { platform_.StopProfileWatcher();platform_.ForgetProfileWatcher(); }
}
void SonarPenController::ApplyProfile(const std::string& json) noexcept {
    auto& q=state_;q.S=false;q.o0=false;
    try {
        if(json.empty()) return;
        const SonarProfileDocument profile(json);
        if(profile.Has("minAmp") && profile.Has("maxAmp")) {
            q.S=true;q.T=float(profile.Double("minAmp"));q.U=float(profile.Double("maxAmp"));
            if(q.T==0 && q.U==0) q.S=false;
        }
        if(profile.Has("maxVol")) {
            q.p0=float(profile.Double("maxVol"));
            if(q.p0<100) { q.o0=true;ConfigureVolume(); }
        }
        if(profile.Has("lowFeq")) { q.h=250;q.t0=true;q.u0=true; }
    } catch(...) {} // Preserve the source's partial assignments.
}
void SonarPenController::LoadManualProfile() noexcept {
    try {
        const auto text=platform_.ReadManualProfile();if(!text || text->empty()) return;
        const SonarProfileDocument profile(*text);auto& q=state_;
        if(profile.Has("minAmp") && profile.Has("maxAmp")) { q.S=true;q.T=float(profile.Double("minAmp"));q.U=float(profile.Double("maxAmp")); }
        if(profile.Has("maxVol")) {
            q.p0=float(profile.Double("maxVol"));if(q.p0<100) { q.o0=true;return; }
        }
        q.o0=false;
    } catch(...) {}
}
void SonarPenController::ProfileChanged() {
    if(state_.k && !state_.F0) { state_.S=false;LoadManualProfile(); }
    state_.F0=false;
}
void SonarPenController::PresetApply() {
    auto& q=state_;q.S=true;q.T=q.I0[0];q.U=q.I0[1];q.p0=q.I0[2];q.W=true;q.o0=q.p0<100;
}
bool SonarPenController::DevicesRefresh() {
    if(!platform_.HasAudioService()) return false;
    auto& q=state_;q.H=false;
    if(!platform_.ModernAudioDevices()) { q.H=platform_.LegacyHeadsetConnected();return true; }
    for(const auto& device:platform_.AudioDevices(3)) {
        if(device.type==22) {
            if(!device.product) throw std::runtime_error("Missing SonarPen audio product label");
            if(device.product->find("SP1")!=std::string::npos) { q.H=true;q.s=true; }
        } else if(device.type==3 || device.type==4) {
            for(const auto& product:platform_.UsbProducts()) {
                if(!product) throw std::runtime_error("Missing SonarPen USB product label");
                if(product->find("SP1")!=std::string::npos) { q.s=true;break; }
            }
            q.H=true;
        }
    }
    return true; // No connected headset is a waiting state, not initialization failure.
}
void SonarPenController::PlugDevicesRefresh() {
    if(!platform_.ModernAudioDevices() || !platform_.HasAudioService()) return;
    auto& q=state_;bool usb_headset=false;
    for(const auto& device:platform_.AudioDevices(3)) if(device.type==22) {
        if(!device.product) throw std::runtime_error("Missing SonarPen audio product label");
        if(device.product->find("SP1")!=std::string::npos) { q.s=true;q.h=1000; }
        else q.H=false;
        usb_headset=true;
    }
    if(usb_headset) return;
    for(const auto& product:platform_.UsbProducts()) {
        if(!product) throw std::runtime_error("Missing SonarPen USB product label");
        if(product->find("SP1")!=std::string::npos) { q.s=true;return; }
    }
}
void SonarPenController::HeadsetChanged(bool connected) {
    const bool previous=state_.H;state_.H=connected;
    if(previous!=connected) {
        if(connected) PlugDevicesRefresh();
        else { StopPlayingTrack();RestoreVolume(); }
    }
}
bool SonarPenController::Key(uint32_t code,uint32_t action) {
    if((code!=79 && code!=85) || !state_.r) return false;
    state_.u=action==0;if(state_.u && state_.C) platform_.PostButton();return true;
}
void SonarPenController::SteadySamples(const int16_t* samples,uint32_t count) {
    if(!samples || !count) return;
    const auto amplitude=SonarSteadyAmplitude(samples,count);if(amplitude>0) state_.a0=amplitude;
    state_.Z=true;UpdatePressure();
}
void SonarPenController::WorkerStart() {
    auto& q=state_;q.l=0;q.m=0;q.u=false;q.r=false;q.A=true;q.I=20;q.B0=false;q.k=true;
    ++q.w0;platform_.CreateWorker(q.w0);platform_.StartWorker();
}
int32_t SonarPenController::Start() { state_.t0=state_.u0;state_.B0=false;return StartImpl(); }
int32_t SonarPenController::StartImpl() {
    auto& q=state_;q.t=platform_.IsChromeOs();
    const bool loaded=platform_.ProfileLoaded();
    if(loaded) {
        const auto json=platform_.ReadProfile();
        if(platform_.LowFrequencySaved()) { q.h=250;q.t0=true; }
        if(json.empty()) { q.S=false;q.o0=false; }
        else try {
            const SonarProfileDocument profile(json);
            if(profile.Has("minAmp") && profile.Has("maxAmp")) { q.S=true;q.T=float(profile.Double("minAmp"));q.U=float(profile.Double("maxAmp")); }
            else q.S=false;
            if(profile.Has("maxVol")) { q.p0=float(profile.Double("maxVol"));q.o0=q.p0<100; }
            else q.o0=false;
            if(profile.Has("lowFeq")) q.h=250;
        } catch(...) {}
    }
    if(q.E0 && !loaded && platform_.LegacyProfileFiles()) {
        if(platform_.CreateProfileWatcher()) platform_.StartProfileWatcher();
    }
    Status(0);RegisterHeadset();
    if(!DevicesRefresh()) { q.Q=2;Status(-1);return q.Q; }
    try { if(!platform_.RecordingAllowed()) { q.Q=1;Status(-1);return 1; } } catch(...) {}
    if(!platform_.HasMediaSession()) {
        platform_.CreateMediaSession("SONICPEN");platform_.MediaSessionFlags(3);platform_.MediaSessionCallback();
        platform_.MediaSessionPlayback(SonarPlaybackState{});platform_.MediaSessionFlags(3);
    }
    platform_.MediaSessionActive(true);
    if(q.H0) PresetApply();else if(!loaded && platform_.LegacyProfileFiles()) LoadManualProfile();
    q.k=true;q.r=false;q.l=0;Status(1);
    q.B=platform_.HasAudioService()?platform_.CurrentVolume():0;
    WorkerStart();q.Q=0;
    if(!loaded && platform_.LegacyProfileFiles()) StopProfileWatcher();
    return 0;
}
void SonarPenController::Stop() {
    StopProfileWatcher();auto& q=state_;q.k=false;
    if(platform_.HasWorker()) {
        if(platform_.WorkerAlive()) { try { platform_.InterruptWorker(); } catch(...) {} }
        platform_.ForgetWorker();
    }
    StopRecording();if(platform_.HasMediaSession()) platform_.MediaSessionActive(false);
    RestoreVolume();StopPlayingTrack();UnregisterHeadset();
    q.B0=false;q.l=0;q.m=0;q.u=false;q.r=false;Status(4);q.Q=-1;
}
}
