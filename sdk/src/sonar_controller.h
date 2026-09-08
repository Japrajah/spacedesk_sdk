#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "sonar_motion.h"

namespace sd::native {
struct SonarProfileEnvironment;
// Role aliases for the numeric/control portion of APK V0.g (SonarPen 3.11).
// Field letters are original DEX names, not recovered semantic class names.
// The Android BroadcastReceiver and its platform registration are outside this
// portable core; SonarPlatform is an SDK adapter contract, not an APK base class.
struct SonarReading { float amplitude=0,frequency=0,level=0; };
struct SonarTrackFormat {
    uint32_t sample_rate=44100,channels=2,sample_bits=16,buffer_bytes=176400;
    uint32_t mode=0; // Original MODE_STATIC; the provider owns platform construction.
};
struct SonarRecordFormat {
    uint32_t sample_rate=44100,channels=2,sample_bits=16,buffer_bytes;
    explicit SonarRecordFormat(int32_t minimum_bytes):buffer_bytes(uint32_t(minimum_bytes>2940?minimum_bytes:2940)) {}
};
struct SonarAudioDevice { uint32_t type=0;std::optional<std::string> product; }; // Original type and nullable product label.
struct SonarPlaybackState {
    uint64_t actions=0x206;int32_t state=3;int64_t position=0;float speed=0;int64_t updated=1;
};
struct SonarPenState {
    float h=9000;                         // Tone frequency parameter.
    bool k=false; int32_t l=0;             // Worker enabled; calibration counter.
    float m=0,n=0,p=0;                     // Pressure, preceding pressure, amplitude.
    bool r=false,s=false,t=false,u=false,v=false;
    float w=405,y=0,z=0;                   // Adaptive pressure bounds.
    bool A=true,C=false; int32_t B=0,D=-1;  // Calibrating, listener, saved volume, contact id.
    bool H=false; int32_t I=20,R=0;         // Device present, delay count, raw status.
    bool S=false; float T=0,U=0,V=0;        // Manual profile and activation threshold.
    bool W=false; double X=0; bool Z=false;
    float a0=0,b0=0,c0=0,d0=0; int32_t e0=0;
    float f0=0,g0=0,h0=0,i0=0; int32_t j0=0,m0=0; // Low-reading counter; original acquisition error.
    bool o0=false,q0=false;float p0=80,r0=80; // Profile/override volume percentages.
    bool t0=true,B0=false,D0=false,G0=true;
    bool u0=true,l0=false,E0=true,F0=false,H0=false;
    int32_t Q=-1;uint64_t w0=0;
    std::array<float,3> I0{};
    int32_t U0=0;
    int32_t E=-1,q=2,z0=0;
    float F=999,G=999,Y=0;
    int64_t J=0,T0=0;
    bool K=true,L=false,M=false,N=false,A0=false,k0=true,o=false,s0=false;
    std::array<float,4> x0{},y0{};
};
class SonarPlatform {
public:
    virtual ~SonarPlatform()=default;
    // Default implementation ports A(): raw device reads, FFT during calibration,
    // steady RMS otherwise, and the original error/partial-assignment behavior.
    // Overrides may supply recorded measurements for deterministic control traces.
    virtual bool Read(bool calibration,SonarPenState&);
    virtual bool HasAudioService()=0; // Source Context.getSystemService("audio") availability.
    virtual int32_t MaximumVolume()=0;
    virtual int32_t CurrentVolume()=0;
    virtual void SetVolume(int32_t)=0;
    virtual void AdjustVolumeUp()=0;
    virtual bool HasTrack() const=0;
    virtual bool TrackPlaying() const=0;
    virtual void CreateTrack(const SonarTrackFormat&)=0;
    virtual int32_t TrackState()=0; // Original 0 uninitialized, 1 initialized, 2 empty static buffer.
    virtual int32_t WriteTrack(const int16_t*,uint32_t count)=0;
    virtual int32_t SetTrackLoop(int32_t start,int32_t end,int32_t count)=0;
    virtual float MaximumTrackGain()=0;
    virtual int32_t SetTrackGain(float)=0;
    virtual void PlayTrack()=0;
    virtual void StopTrack()=0;
    virtual void ReleaseTrack()=0;
    virtual bool HasRecorder() const=0;
    // Immutable format metadata measured before controller construction, like
    // the APK's static W0=max(getMinBufferSize(...),1470*2).
    virtual int32_t MinimumRecordBytes() const noexcept=0;
    virtual void CreateRecorder(const SonarRecordFormat&)=0;
    virtual int32_t RecordingState()=0;
    virtual void StartRecorder()=0;
    virtual int32_t ReadBytes(uint8_t*,uint32_t capacity)=0;
    virtual int32_t ReadSamples(int16_t*,uint32_t capacity)=0;
    virtual void StopRecorder()=0;
    virtual void ReleaseRecorder()=0;
    virtual void ForgetRecorder() noexcept=0; // Original j=null reference assignment.
    virtual void SaveLowFrequency()=0;
    virtual void SaveReadings(float silent,float left,float right,float noise,float frequency)=0;
    virtual bool HonorOrHuawei() const=0;
    virtual void Status(int32_t)=0;
    virtual void PostButton()=0;
    virtual bool IsChromeOs()=0;
    virtual bool ModernAudioDevices() const noexcept=0; // Source API >=23.
    virtual bool LegacyProfileFiles() const noexcept=0; // Source API <28.
    virtual std::vector<SonarAudioDevice> AudioDevices(uint32_t direction)=0;
    virtual std::vector<std::optional<std::string>> UsbProducts()=0;
    virtual bool LegacyHeadsetConnected()=0;
    virtual bool RecordingAllowed()=0;
    virtual bool ProfileLoaded()=0;
    virtual SonarProfileEnvironment* ProfileEnvironment() noexcept { return nullptr; }
    virtual std::string ReadProfile()=0;
    virtual bool LowFrequencySaved()=0;
    virtual std::optional<std::string> ReadManualProfile()=0;
    virtual bool CreateProfileWatcher()=0;
    virtual bool HasProfileWatcher() const=0;
    virtual void StartProfileWatcher()=0;
    virtual void StopProfileWatcher()=0;
    virtual void ForgetProfileWatcher() noexcept=0;
    virtual void RegisterHeadset()=0;
    virtual void UnregisterHeadset()=0;
    virtual bool HasMediaSession() const=0;
    virtual void CreateMediaSession(const char* tag)=0;
    virtual void MediaSessionFlags(uint32_t)=0;
    virtual void MediaSessionCallback()=0;
    virtual void MediaSessionPlayback(const SonarPlaybackState&)=0;
    virtual void MediaSessionActive(bool)=0;
    virtual bool HasWorker() const=0;
    virtual bool WorkerAlive()=0;
    virtual void CreateWorker(uint64_t generation)=0;
    virtual void StartWorker()=0;
    virtual void InterruptWorker()=0;
    virtual void ForgetWorker() noexcept=0; // Clear the shared reference; must not join the calling worker.
};
class SonarPenController {
    SonarPenState state_;
    SonarPlatform& platform_;
    void Status(int32_t);                   // V0.g.k
    void StopPlayingTrack();               // V0.g.r0
    void StopRecording();                  // V0.g.s0
    void CalibrationFailure();             // V0.g.j
    float Threshold(float) const;          // V0.g.a
    void StopProfileWatcher();             // V0.g.F
    void RegisterHeadset();                // V0.g.d0
    void UnregisterHeadset();              // V0.g.t0
    void LoadManualProfile() noexcept;     // V0.g.T
    void PresetApply();                    // V0.g.t
    void WorkerStart();                    // V0.g.p0
    int32_t StartImpl();                   // V0.g.q0
    void PlugDevicesRefresh();             // V0.g.Y
public:
    // State has one owner. A real platform must serialize worker/device/UI
    // callbacks; clearing the source's worker reference is not a thread join.
    explicit SonarPenController(SonarPlatform& platform);
    SonarPenController(const SonarPenController&)=delete;
    SonarPenController& operator=(const SonarPenController&)=delete;
    SonarPenState& State() noexcept { return state_; }
    const SonarPenState& State() const noexcept { return state_; }
    void ConfigureVolume();                // V0.g.W
    void RestoreVolume();                  // V0.g.e0
    void CreateTone(bool right);           // V0.g.n
    void Calibrate() noexcept;              // V0.g.g0
    void UpdatePressure() noexcept;         // V0.g.i0
    int32_t Start();                        // V0.g.k0
    void Stop();                           // V0.g.n0
    bool DevicesRefresh();                 // V0.g.u0
    void ApplyProfile(const std::string&) noexcept; // V0.g.u (not identical to startup profile handling).
    void ProfileChanged();                 // V0.g.a0
    bool ReceiveProfile(const std::string& to,const std::string& sender,const std::string& text);
    void HeadsetChanged(bool connected);   // V0.g.onReceive HEADSET_PLUG branch.
    bool Key(uint32_t code,uint32_t action); // V0.g.P: Android headset/media key 79/85.
    SonarMotionResult Motion(SonarMotionEvent&,SonarMotionClock&); // V0.g.o0.
    void SteadySamples(const int16_t*,uint32_t count); // A()+i0 work while the UI wait cooperatively yields.
    // One original RunnableC0020g iteration. Caller owns thread/scheduling.
    // -1 ends the loop, 50/100 request the original absent/suspended sleep.
    int32_t Tick();
};
}
