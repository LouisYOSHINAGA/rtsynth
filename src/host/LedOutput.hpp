#pragma once

namespace rtsynth {

// A bank of on/off indicators (LEDs on an I/O expander, GPIO, a fake in
// the self-test...). setLed() may be called from any thread and must not
// block: implementations record the wanted state and push it to the
// hardware from their own thread (see Mcp23017Panel).
class LedOutput {
public:
    virtual ~LedOutput() = default;

    virtual int numChannels() const = 0;
    virtual void setLed(int channel, bool on) = 0;
};

}  // namespace rtsynth
