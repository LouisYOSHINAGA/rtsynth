#pragma once

#include <cstdint>

namespace rtsynth {

// Quadrature decoder for one rotary encoder (EC11 and friends): feed it
// the current state of the A/B lines, get back full detent steps.
// Separated from the I/O plumbing so it can be unit-tested offline, and
// shared by both encoder front ends: GpioEncoderInput feeds it from edge
// events, Mcp23017Panel from sampled port snapshots (an invalid two-bit
// jump between samples simply counts as no movement).
//
// For encoders without clicks (ALPS EC12 non-detent and the like) a
// "detent" is just the resolution chosen: 4 = one step per full quadrature
// cycle (= the datasheet's pulses per revolution), 1 = every edge counts.
class QuadratureDecoder {
public:
    // stepsPerDetent: quarter-cycles per mechanical click (4 on an EC11)
    explicit QuadratureDecoder(int stepsPerDetent = 4)
        : stepsPerDetent_(stepsPerDetent){}

    void reset(uint8_t abState){
        state_ = abState & 0x3;
        quarters_ = 0;
    }

    // abState = (A << 1) | B. Returns -1/0/+1 whole detents.
    int onState(uint8_t abState){
        abState &= 0x3;
        // transition table: +1 = clockwise quarter step, -1 = ccw,
        // 0 = no movement or invalid (bounce) transition
        static constexpr int8_t kDelta[16] = {
             0, +1, -1,  0,
            -1,  0,  0, +1,
            +1,  0,  0, -1,
             0, -1, +1,  0,
        };
        quarters_ += kDelta[(state_ << 2) | abState];
        state_ = abState;
        if(quarters_ >= stepsPerDetent_){
            quarters_ -= stepsPerDetent_;
            return +1;
        }
        if(quarters_ <= -stepsPerDetent_){
            quarters_ += stepsPerDetent_;
            return -1;
        }
        return 0;
    }

private:
    int stepsPerDetent_;
    uint8_t state_ = 0;
    int quarters_ = 0;
};

}  // namespace rtsynth
