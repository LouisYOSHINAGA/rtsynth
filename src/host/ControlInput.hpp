#pragma once

namespace rtsynth {

// Abstraction over a bank of physical analog controls (pots, sliders,
// faders...). Implementations talk to the actual hardware — an SPI/I2C
// ADC, GPIO, a mock for tests — and report each channel normalized to
// [0, 1]. read() is only ever called from the ControlLoop's polling
// thread, so implementations need no locking of their own.
class ControlInput {
public:
    virtual ~ControlInput() = default;

    virtual const char* name() const = 0;
    virtual int numChannels() const = 0;

    // Returns false on a failed read (device unplugged, bus error, ...);
    // normalizedOut is left untouched in that case.
    virtual bool read(int channel, float& normalizedOut) = 0;
};

}  // namespace rtsynth
