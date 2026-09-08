#pragma once
#include "spacedesk_sdk.h"
#include "sonar_fft.h"
#include <vector>

namespace sd::native {
// SDK role aliases for APK W0.b spectrum accumulator and W0.a block analyzer.
// Original classes have no explicit superclass or virtual methods.
class SonarSpectrum {
    uint32_t size_,fill_=0,windows_=0;
    SonarRealFft fft_;
    std::vector<double> accumulated_,power_,average_,decibels_,input_,transformed_,window_;
    std::vector<std::vector<double>> history_;
    uint32_t history_index_=0;
    void Power();
public:
    explicit SonarSpectrum(uint32_t size);
    void Input(const int16_t*,uint32_t);
    double Frequency();
    uint32_t Windows() const noexcept { return windows_; }
};
class SonarSignalAnalyzer {
    std::vector<int16_t> samples_;
    std::vector<double> unused_double_cache_; // W0.a.c initializes/resets null; retained empty.
    int32_t peak_=0;
    double frequency_=0,level_=0;
public:
    SonarSignalAnalyzer()=default;
    void Reset(const int16_t*,uint32_t);
    sd_sonar_signal Analyze();
};
float SonarPressureCurve(uint32_t curve,float amplitude,float minimum,float maximum);
float SonarSteadyAmplitude(const int16_t*,uint32_t count);
void SonarTone(float frequency,uint32_t channel,int16_t* output);
}
