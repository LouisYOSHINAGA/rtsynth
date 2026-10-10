#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ControlInput.hpp"
#include "QuadratureDecoder.hpp"

namespace rtsynth {

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
