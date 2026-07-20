#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "../core/Parameters.hpp"
#include "ControlInput.hpp"

namespace rtsynth {

// Polls physical controls on its own (non-realtime) thread and writes the
// values into Parameters — the hardware-UI equivalent of the MIDI CC path.
// Because Parameters are atomic, the audio thread picks the changes up on
// its next block without any locking. Both control kinds are supported and
// can be mixed freely (e.g. ADC pots on some parameters, encoders on
// others, MIDI CC on the rest — last writer wins, like any synth panel):
//
//   absolute (ControlInput, e.g. MCP3008 pots): two stages keep a noisy
//   pot from spamming or "zipping" a parameter — an exponential moving
//   average filters ADC noise, and a write threshold (~2 LSB of a 10-bit
//   ADC) suppresses idle jitter, so the Parameter is only touched when
//   the knob really moved.
//
//   relative (RelativeControlInput, e.g. GPIO rotary encoders): each
//   detent step nudges the parameter by `stepSize` of its normalized
//   range; no filtering is needed because encoders are already discrete.
//
// (Per-sample smoothing against zipper noise is the Processor's job —
// see dsp/SmoothedValue.hpp; this class only tames the control rate.)
class ControlLoop {
public:
    // Either input may be null when that control kind is not used.
    explicit ControlLoop(ControlInput* absoluteInput = nullptr,
                         RelativeControlInput* relativeInput = nullptr)
        : absolute_(absoluteInput), relative_(relativeInput){}
    ~ControlLoop(){ stop(); }

    // Map an absolute-input channel onto a parameter. Call before start().
    void addMapping(int channel, Parameter* parameter){
        absoluteSlots_.push_back({channel, parameter, 0.0f, -1.0f, false});
    }

    // Map a relative-input channel onto a parameter; one detent step moves
    // the normalized value by stepSize. Call before start().
    void addRelativeMapping(int channel, Parameter* parameter,
                            float stepSize = kDefaultStepSize){
        relativeSlots_.push_back({channel, parameter, stepSize});
    }

    void start(int pollRateHz = 100){
        if(running_.exchange(true)){
            return;
        }
        thread_ = std::thread([this, pollRateHz]{
            const auto period =
                std::chrono::microseconds(1000000 / std::max(1, pollRateHz));
            while(running_.load()){
                pollOnce();
                std::this_thread::sleep_for(period);
            }
        });
    }

    void stop(){
        if(!running_.exchange(false)){
            return;
        }
        if(thread_.joinable()){
            thread_.join();
        }
    }

    // One synchronous poll of every mapping (also used by the self-test).
    void pollOnce(){
        pollAbsolute();
        pollRelative();
    }

private:
    static constexpr float kSmoothing = 0.5f;        // EMA coefficient per poll
    static constexpr float kWriteThreshold = 0.002f; // ~2 LSB at 10 bit
    static constexpr float kDefaultStepSize = 0.01f; // 100 detents = full range

    struct AbsoluteSlot {
        int channel;
        Parameter* parameter;
        float filtered;
        float lastWritten;
        bool primed;
    };

    struct RelativeSlot {
        int channel;
        Parameter* parameter;
        float stepSize;
    };

    void pollAbsolute(){
        if(absolute_ == nullptr){
            return;
        }
        for(AbsoluteSlot& slot : absoluteSlots_){
            float raw = 0.0f;
            if(!absolute_->read(slot.channel, raw)){
                continue;
            }
            if(!slot.primed){
                // first successful read: adopt the physical knob position
                slot.filtered = raw;
                slot.primed = true;
            }else{
                slot.filtered += kSmoothing * (raw - slot.filtered);
            }
            if(std::abs(slot.filtered - slot.lastWritten) > kWriteThreshold){
                slot.parameter->setNormalized(slot.filtered);
                slot.lastWritten = slot.filtered;
            }
        }
    }

    void pollRelative(){
        if(relative_ == nullptr){
            return;
        }
        for(RelativeSlot& slot : relativeSlots_){
            const int delta = relative_->readDelta(slot.channel);
            if(delta != 0){
                slot.parameter->setNormalized(
                    slot.parameter->getNormalized()
                    + static_cast<float>(delta) * slot.stepSize);
            }
        }
    }

    ControlInput* absolute_;
    RelativeControlInput* relative_;
    std::vector<AbsoluteSlot> absoluteSlots_;
    std::vector<RelativeSlot> relativeSlots_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace rtsynth
