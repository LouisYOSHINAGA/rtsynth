#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "LedOutput.hpp"

namespace rtsynth {

// LEDs on a daisy chain of 74HC595 shift registers, written over SPI —
// eight outputs per chip, any number of chips, three wires from the Pi.
// LEDs only need outputs, so this keeps the MCP23017s free for inputs.
//
// Wiring (every 595 at 3.3 V: at 5 V its inputs would not see the Pi's
// 3.3 V signals as high reliably):
//
//   Pi MOSI  GPIO10 (pin 19) -> SER   (pin 14) of chip 0
//   Pi SCLK  GPIO11 (pin 23) -> SRCLK (pin 11) of every chip
//   Pi CE1   GPIO7  (pin 26) -> RCLK  (pin 12) of every chip
//   chip n  QH' (pin 9)      -> SER of chip n+1
//   /OE (pin 13) -> GND,  /SRCLR (pin 10) -> 3V3,  VCC (16) 3V3, GND (8)
//   LED:  Qx -> resistor -> LED -> GND  (active high)
//
// The SPI chip select doubles as the latch: CE1 drops for the transfer and
// rises when it ends, and that rising edge on RCLK moves the whole chain
// to the outputs at once, so LEDs never flicker through shifting states.
// CE0 stays free for the MCP3008.
//
// Channel = output index across the chain: chip * 8 + bit, bit 0 = QA
// ... 7 = QH, chip 0 = the one wired to MOSI.
//
// Threading: setLed() and flush() belong to the ControlLoop thread (the
// panel logic driving the LEDs runs there); open()/close() to main.
class ShiftRegisterLeds : public LedOutput {
public:
    ~ShiftRegisterLeds() override { close(); }

    // setup: chain length, then per-output polarity (before open)
    void setChainLength(int chips);
    void setActiveLow(int channel, bool activeLow);

    bool open(const std::string& devicePath, uint32_t speedHz = 1000000);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    int numChips() const { return static_cast<int>(bytes_.size()); }
    int numChannels() const override { return numChips() * 8; }
    void setLed(int channel, bool on) override;
    void flush() override;

    // the bytes as they would be sent (first byte = last chip), for tests
    std::vector<uint8_t> frame() const;

    static std::string pinName(int channel);  // "sr2.5"

private:
    int fd_ = -1;
    uint32_t speedHz_ = 1000000;
    std::vector<uint8_t> bytes_;      // wanted output level, per chip
    std::vector<uint8_t> invert_;     // active-low outputs, per chip
    std::vector<uint8_t> written_;    // last frame sent
    bool dirty_ = true;
};

}  // namespace rtsynth
