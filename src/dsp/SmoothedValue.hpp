#pragma once

#include <cmath>

namespace rtsynth {

// One-pole parameter smoother ("anti-zipper"). Control values (knobs, MIDI
// CC, automation) arrive in coarse steps; multiplying audio by a stepped
// gain produces audible clicks/zipper noise. Audio code calls tick() once
// per sample so the current value chases the target exponentially, reaching
// ~63% of a step in `timeConstant` seconds.
class SmoothedValue {
public:
    void prepare(double sampleRate, float timeConstantSeconds){
        coeff_ = 1.0f - std::exp(-1.0f / (timeConstantSeconds
                                          * static_cast<float>(sampleRate)));
    }

    // jump immediately (stream start, program change)
    void snap(float value){
        current_ = value;
        target_ = value;
    }

    void setTarget(float value){ target_ = value; }

    float tick(){
        current_ += coeff_ * (target_ - current_);
        return current_;
    }

    float current() const { return current_; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float coeff_ = 1.0f;
};

}  // namespace rtsynth
