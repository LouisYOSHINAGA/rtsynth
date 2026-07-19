#pragma once

#include <cstdint>
#include <string>
#include "ControlInput.hpp"

namespace rtsynth {

// MCP3008: 8-channel, 10-bit SPI ADC — the classic way to give a Raspberry
// Pi analog knobs, since the Pi has no ADC of its own.
//
// Wiring (Pi header / SPI0):
//   MCP3008 VDD, VREF -> 3.3V     CLK  -> GPIO11 (SCLK)
//   MCP3008 AGND, DGND -> GND     DOUT -> GPIO9  (MISO)
//   pot: one leg 3.3V, other GND, DIN  -> GPIO10 (MOSI)
//        wiper -> CH0..CH7        CS   -> GPIO8  (CE0)
//
// Enable SPI first (raspi-config -> Interface Options -> SPI), then pass
// "/dev/spidev0.0" to open(). read() runs on the ControlLoop thread.
class Mcp3008Input : public ControlInput {
public:
    ~Mcp3008Input() override { close(); }

    bool open(const std::string& devicePath, uint32_t speedHz = 1000000);
    void close();

    const char* name() const override { return "MCP3008"; }
    int numChannels() const override { return 8; }
    bool read(int channel, float& normalizedOut) override;

private:
    int fd_ = -1;
    uint32_t speedHz_ = 1000000;
};

}  // namespace rtsynth
