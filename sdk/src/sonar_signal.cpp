#include "sonar_signal.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sd::native {
namespace {
constexpr double pi=3.141592653589793;
double truncate_tenth(double value) { return double(int32_t(value*10.0))/10.0; }
}
SonarSpectrum::SonarSpectrum(uint32_t size):size_(size),fft_(int(size)),accumulated_(size),power_(size),
    average_(size),decibels_(size),input_(size),transformed_(size),window_(size),
    history_(size_t(std::ceil(1.0/size)),std::vector<double>(size)) {
    // W0.b.e: keep the original asin(sin()) window and operation order.
    for(uint32_t i=0;i<size;++i) window_[i]=(std::asin(std::sin((i*pi)/size))/pi)*2.0;
}
void SonarSpectrum::Power() {
    const double scale=4.0/(size_*size_);
    power_[0]=((transformed_[0]*transformed_[0])*scale)/4.0;
    uint32_t bin=1;
    for(uint32_t i=1;i<size_-1;i+=2,++bin)
        power_[bin]=(transformed_[i]*transformed_[i]+transformed_[i+1]*transformed_[i+1])*scale;
    power_[bin]=((transformed_[size_-1]*transformed_[size_-1])*scale)/4.0;
}
void SonarSpectrum::Input(const int16_t* samples,uint32_t count) {
    uint32_t index=0;
    while(index<count) {
        while(fill_<size_ && index<count) input_[fill_++]=samples[index++]/32768.0;
        if(fill_==size_) {
            for(uint32_t i=0;i<size_;++i) transformed_[i]=input_[i]*window_[i];
            fft_.Forward(transformed_);Power();
            history_[history_index_]=power_;history_index_=uint32_t((history_index_+1)%history_.size());
            for(uint32_t i=0;i<size_;++i) accumulated_[i]+=power_[i];
            ++windows_;const uint32_t half=size_/2;
            std::memmove(input_.data(),input_.data()+half,size_t(half)*sizeof(double));fill_=half;
        }
    }
}
double SonarSpectrum::Frequency() {
    if(windows_) {
        for(uint32_t i=0;i<size_;++i) accumulated_[i]/=windows_;
        average_=accumulated_;std::fill(accumulated_.begin(),accumulated_.end(),0.0);windows_=0;
        for(uint32_t i=0;i<size_;++i) decibels_[i]=std::log10(average_[i])*10.0;
    }
    double highest=std::log10(3.814697265625e-6)*20.0,bin=0;
    for(uint32_t i=1;i<size_;++i) if(decibels_[i]>highest) { bin=i;highest=decibels_[i]; }
    const double frequency=(bin*44100.0)/size_;
    // W0.b.a uses integer division for this edge guard (confirmed by DEX).
    const uint32_t step=44100/size_;
    if(step>=frequency || frequency>=22050-step) return frequency;
    const auto center=size_t(std::floor((frequency/44100.0)*size_+0.5));
    const double left=decibels_[center-1],middle=decibels_[center],right=decibels_[center+1];
    const double curvature=((right+left)/2.0)-middle,slope=(right-left)/2.0;
    if(curvature>=0) return frequency;
    const double offset=(-slope)/(curvature*2.0);
    return std::abs(offset)<1.0?frequency+(offset*44100.0)/size_:frequency;
}
void SonarSignalAnalyzer::Reset(const int16_t* samples,uint32_t count) {
    samples_.assign(samples,samples+count);unused_double_cache_.clear();peak_=0;frequency_=level_=0;
}
sd_sonar_signal SonarSignalAnalyzer::Analyze() {
    sd_sonar_signal out{};out.struct_size=sizeof(out);out.sample_count=uint32_t(samples_.size());
    uint32_t size=8192;while(size>samples_.size()) size>>=1;
    int32_t maximum=0,minimum=0;
    for(int16_t sample:samples_) {
        maximum=std::max(maximum,int32_t(sample));minimum=std::min(minimum,int32_t(sample));
    }
    peak_=std::max(maximum,-minimum);out.peak_amplitude=peak_;
    out.steady_amplitude=SonarSteadyAmplitude(samples_.data(),out.sample_count);
    SonarSpectrum spectrum(size);spectrum.Input(samples_.data(),out.sample_count);
    out.fft_size=size;out.fft_windows=spectrum.Windows();
    frequency_=truncate_tenth(spectrum.Frequency());out.frequency_hz=frequency_;
    double percent=(peak_/32767.0)*100.0;if(percent==0) percent=1;
    const double root=std::sqrt(100.0/percent),square=root*root;
    level_=truncate_tenth(((std::min(square,100.0)*-1.0)+1.0)/pi);out.level=level_;
    return out;
}
float SonarSteadyAmplitude(const int16_t* samples,uint32_t count) {
    int64_t squares=0;
    for(uint32_t i=0;i<count;++i) squares+=int32_t(samples[i])*int32_t(samples[i]);
    // V0.g.d: Java promotes long sum to float before dividing by count*2.0f.
    const float mean=float(squares)/(float(count)*2.0f);
    return float(std::sqrt(double(mean)));
}
float SonarPressureCurve(uint32_t curve,float amplitude,float minimum,float maximum) {
    const float fraction=(amplitude-minimum)/(maximum-minimum);
    if(fraction<0) return 0;
    const double value=fraction;
    const double pressure=curve==0?((std::pow(value,3.0)*2.21622)-(std::pow(value,2.0)*1.91606))+(value*.7018)+8.0e-5:
        ((std::pow(value,3.0)*.7752)-(std::pow(value,2.0)*.30993))+(value*.53326)-5.6e-4;
    return float(std::min(pressure,1.0));
}
void SonarTone(float frequency,uint32_t channel,int16_t* output) {
    const double denominator=44100.0/double(frequency);
    for(uint32_t i=0;i<88200;i+=2) {
        const auto sample=int16_t(std::sin((i*6.283185307179586)/denominator)*32767.0);
        output[i]=channel?0:sample;output[i+1]=channel?sample:0;
    }
}
}

extern "C" {
sd_result SD_CALL sd_sonar_analyze_pcm16(const int16_t* samples,uint32_t count,sd_sonar_signal* result) {
    if(!samples || count<2 || count>65536 || !result || result->struct_size!=sizeof(*result)) return SD_INVALID_ARGUMENT;
    try {
        sd::native::SonarSignalAnalyzer analyzer;analyzer.Reset(samples,count);const auto value=analyzer.Analyze();
        *result=value;return SD_OK;
    } catch(const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
      catch(...) { return SD_INTERNAL_ERROR; }
}
sd_result SD_CALL sd_sonar_pressure_curve(uint32_t curve,float amplitude,float minimum,float maximum,float* pressure) {
    if(!pressure || curve>1 || !std::isfinite(amplitude) || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum<=minimum)
        return SD_INVALID_ARGUMENT;
    *pressure=sd::native::SonarPressureCurve(curve,amplitude,minimum,maximum);return SD_OK;
}
sd_result SD_CALL sd_sonar_generate_tone(float frequency,uint32_t channel,int16_t* output,uint32_t capacity,uint32_t* sample_count) {
    if(!sample_count || !std::isfinite(frequency) || frequency<=0 || frequency>22050 || channel>1 || (!output && capacity))
        return SD_INVALID_ARGUMENT;
    *sample_count=88200;
    if(!output) return SD_OK;
    if(capacity<88200) return SD_INVALID_ARGUMENT;
    sd::native::SonarTone(frequency,channel,output);return SD_OK;
}
}
