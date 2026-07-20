#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ControlInput.hpp"

namespace rtsynth {

// Quadrature decoder for one rotary encoder (EC11 and friends): feed it
// the current state of the A/B lines, get back full detent steps.
// Separated from the GPIO plumbing so it can be unit-tested offline.
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

// Rotary encoders on Raspberry Pi GPIO pins, exposed as a
// RelativeControlInput (one channel per encoder, in addEncoder() order).
//
// Uses the kernel's GPIO character device (/dev/gpiochipN, uapi v2) — no
// external library. Lines are requested with internal pull-ups and edge
// events on both lines; a background thread poll()s the event fds and
// runs the quadrature decoders, accumulating detent steps into atomic
// counters that readDelta() consumes from the ControlLoop thread.
//
// Wiring (EC11): A and B to two GPIOs, C (common) to GND. No external
// resistors needed thanks to the requested pull-ups.
class GpioEncoderInput : public RelativeControlInput {
public:
    ~GpioEncoderInput() override { close(); }

    // Declare encoders first, then open(). Returns this channel's index.
    int addEncoder(unsigned int pinA, unsigned int pinB);

    // Requests the GPIO lines and starts the event thread.
    bool open(const std::string& chipPath = "/dev/gpiochip0");
    void close();

    const char* name() const override { return "GPIO encoders"; }
    int numChannels() const override { return static_cast<int>(encoders_.size()); }
    int readDelta(int channel) override;

private:
    struct Encoder {
        unsigned int pinA = 0;
        unsigned int pinB = 0;
        int fd = -1;                      // per-encoder line request
        uint8_t abState = 0;              // event thread only
        QuadratureDecoder decoder;
        std::atomic<int> steps{0};
    };

    void eventThread();

    std::vector<std::unique_ptr<Encoder>> encoders_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace rtsynth
