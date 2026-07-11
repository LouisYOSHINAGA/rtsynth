#pragma once

#include <algorithm>

namespace rtsynth {

// Linear-segment ADSR (Attack / Decay / Sustain / Release) envelope,
// ticked once per sample:
//
//   level
//   1.0 |    /\_
//       |   /   \_____________
//       |  /    :             \_
//       | /     :  sustain      \_
//   0.0 |/______:_________________\__ time
//         attack decay      ^      release
//                        noteOff()
//
// Click-avoidance rules baked into this implementation:
//   - noteOn() attacks from the *current* level (not from zero), so
//     retriggering a still-sounding voice never jumps
//   - the release rate is computed from the level at noteOff() time, so
//     release always reaches zero in `release` seconds regardless of level
//   - fastRelease() is a ~3 ms fade for voice stealing
//   - the sustain stage ramps toward the sustain level, so automating the
//     sustain parameter while a note is held stays smooth
class AdsrEnvelope {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setSampleRate(double sampleRate){ sampleRate_ = sampleRate; }

    // times in seconds, sustain in [0, 1]
    void setParameters(float attack, float decay, float sustain, float release){
        attackRate_ = 1.0f / secondsToSamples(attack);
        decayRate_ = 1.0f / secondsToSamples(decay);
        sustainLevel_ = std::clamp(sustain, 0.0f, 1.0f);
        releaseTime_ = release;
    }

    void noteOn(){
        stage_ = Stage::Attack;  // ramp up from wherever we are now
    }

    void noteOff(){
        if(stage_ == Stage::Idle){
            return;
        }
        releaseRate_ = level_ / secondsToSamples(releaseTime_);
        stage_ = Stage::Release;
    }

    // fast fade used when a voice gets stolen (a few ms, click-free)
    void fastRelease(){
        releaseRate_ = level_ / secondsToSamples(kFastReleaseSeconds);
        stage_ = Stage::Release;
    }

    void reset(){
        stage_ = Stage::Idle;
        level_ = 0.0f;
    }

    bool isActive() const { return stage_ != Stage::Idle; }
    bool isReleasing() const { return stage_ == Stage::Release; }
    Stage stage() const { return stage_; }
    float level() const { return level_; }

    float tick(){
        switch(stage_){
            case Stage::Attack:
                level_ += attackRate_;
                if(level_ >= 1.0f){
                    level_ = 1.0f;
                    stage_ = Stage::Decay;
                }
                break;
            case Stage::Decay:
                level_ -= decayRate_;
                if(level_ <= sustainLevel_){
                    level_ = sustainLevel_;
                    stage_ = Stage::Sustain;
                }
                break;
            case Stage::Sustain:
                // ramp toward the (possibly automated) sustain level instead
                // of jumping, so live parameter changes stay click-free
                if(level_ < sustainLevel_){
                    level_ = std::min(level_ + attackRate_, sustainLevel_);
                }else{
                    level_ = std::max(level_ - decayRate_, sustainLevel_);
                }
                break;
            case Stage::Release:
                level_ -= releaseRate_;
                if(level_ <= 0.0f){
                    level_ = 0.0f;
                    stage_ = Stage::Idle;
                }
                break;
            case Stage::Idle:
                break;
        }
        return level_;
    }

private:
    static constexpr float kFastReleaseSeconds = 0.003f;
    static constexpr float kMinSeconds = 0.001f;  // avoid divide-by-zero / clicks

    float secondsToSamples(float seconds) const {
        return std::max(seconds, kMinSeconds) * static_cast<float>(sampleRate_);
    }

    double sampleRate_ = 44100.0;
    Stage stage_ = Stage::Idle;
    float level_ = 0.0f;
    float attackRate_ = 0.0f;
    float decayRate_ = 0.0f;
    float sustainLevel_ = 1.0f;
    float releaseTime_ = 0.01f;
    float releaseRate_ = 0.0f;
};

}  // namespace rtsynth
