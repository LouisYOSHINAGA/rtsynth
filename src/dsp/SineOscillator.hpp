#pragma once

#include <cmath>

namespace rtsynth {

class SineOscillator {
public:
    void setSampleRate(double sampleRate){ sampleRate_ = sampleRate; }

    void setFrequency(double freq){
        phaseIncrement_ = kTwoPi * freq / sampleRate_;
    }

    void resetPhase(){ phase_ = 0.0; }

    float tick(){
        const float sample = static_cast<float>(std::sin(phase_));
        phase_ += phaseIncrement_;
        while(phase_ >= kTwoPi){
            phase_ -= kTwoPi;
        }
        return sample;
    }

private:
    static constexpr double kTwoPi = 2.0 * M_PI;
    double sampleRate_ = 44100.0;
    double phase_ = 0.0;
    double phaseIncrement_ = 0.0;
};

}  // namespace rtsynth
