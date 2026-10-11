#pragma once

namespace rtsynth {

// A bank of on/off indicators (LEDs on an I/O expander, a shift-register
// chain, a fake in the self-test...).
//
// setLed() only records the wanted state and never blocks. flush() pushes
// recorded changes to the hardware; it is called from the ControlLoop
// thread after a batch of setLed() calls (EgEditPanel does this), so one
// SPI transfer carries every LED that changed in that tick. Outputs that
// refresh themselves from their own thread (Mcp23017Panel) leave it empty.
class LedOutput {
public:
    virtual ~LedOutput() = default;

    virtual int numChannels() const = 0;
    virtual void setLed(int channel, bool on) = 0;
    virtual void flush(){}
};

}  // namespace rtsynth
