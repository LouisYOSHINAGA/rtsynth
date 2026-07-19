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

// Polls a ControlInput on its own (non-realtime) thread and writes the
// values into Parameters — the hardware-UI equivalent of the MIDI CC path.
// Because Parameters are atomic, the audio thread picks the changes up on
// its next block without any locking.
//
// Two stages keep a noisy pot from spamming or "zipping" a parameter:
//   1. an exponential moving average filters ADC noise per poll
//   2. a write threshold (~2 LSB of a 10-bit ADC) suppresses idle jitter,
//      so the Parameter is only touched when the knob actually moved
// (Per-sample smoothing against zipper noise is the Processor's job —
// see dsp/SmoothedValue.hpp; this class only tames the control rate.)
class ControlLoop {
public:
    explicit ControlLoop(ControlInput& input) : input_(input){}
    ~ControlLoop(){ stop(); }

    // Map an input channel onto a parameter. Call before start().
    void addMapping(int channel, Parameter* parameter){
        slots_.push_back({channel, parameter, 0.0f, -1.0f, false});
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
        for(Slot& slot : slots_){
            float raw = 0.0f;
            if(!input_.read(slot.channel, raw)){
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

private:
    static constexpr float kSmoothing = 0.5f;        // EMA coefficient per poll
    static constexpr float kWriteThreshold = 0.002f; // ~2 LSB at 10 bit

    struct Slot {
        int channel;
        Parameter* parameter;
        float filtered;
        float lastWritten;
        bool primed;
    };

    ControlInput& input_;
    std::vector<Slot> slots_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace rtsynth
