#pragma once

namespace rtsynth {

// Abstractions over banks of physical controls. Two kinds exist, matching
// the two kinds of hardware:
//
//   ControlInput          absolute position in [0, 1] — pots, sliders,
//                         faders read through an ADC. The knob position IS
//                         the value.
//   RelativeControlInput  signed steps since last read — rotary encoders,
//                         jog wheels. The knob reports movement, and the
//                         value is wherever previous movements left it.
//
// Both are polled from the ControlLoop only, so implementations need no
// locking toward the loop (they may still synchronize with their own
// hardware threads internally, as GpioEncoderInput does).
class ControlInput {
public:
    virtual ~ControlInput() = default;

    virtual const char* name() const = 0;
    virtual int numChannels() const = 0;

    // Returns false on a failed read (device unplugged, bus error, ...);
    // normalizedOut is left untouched in that case.
    virtual bool read(int channel, float& normalizedOut) = 0;
};

class RelativeControlInput {
public:
    virtual ~RelativeControlInput() = default;

    virtual const char* name() const = 0;
    virtual int numChannels() const = 0;

    // Signed detent steps accumulated since the previous call (0 = idle).
    virtual int readDelta(int channel) = 0;
};

}  // namespace rtsynth
