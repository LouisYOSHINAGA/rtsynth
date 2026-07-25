#pragma once

#include <cstdint>
#include <string>

#include "TextDisplay.hpp"

namespace rtsynth {

// 16x2 character LCD (HD44780) behind the ubiquitous PCF8574 I2C
// "backpack" — the cheapest way to put text on a hardware synth.
//
// Wiring (backpack module): VCC -> 5V, GND -> GND, SDA -> GPIO2 (SDA1),
// SCL -> GPIO3 (SCL1). The Pi's I2C pins are 3.3V but the backpack only
// drives them low (open-drain), so this pairing is safe as long as the
// module's pull-ups go to 3.3V or are removed — the common HW-061 module
// works out of the box. Enable I2C via raspi-config, then find the
// address (usually 0x27 or 0x3F) with: i2cdetect -y 1
//
// Uses the kernel I2C character device (/dev/i2c-1) directly — no
// external library. The backpack maps PCF8574 bits to the LCD as:
//   P0=RS  P1=RW  P2=EN  P3=backlight  P4..P7=D4..D7 (4-bit mode)
class I2cLcd1602 : public TextDisplay {
public:
    ~I2cLcd1602() override { close(); }

    bool open(const std::string& busPath = "/dev/i2c-1", uint8_t address = 0x27);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    int columns() const override { return 16; }
    int rows() const override { return 2; }
    void writeLine(int row, const std::string& text) override;
    void clear() override;

private:
    void writeByte(uint8_t value, bool isData);
    void writeNibble(uint8_t nibble, bool isData);
    void writeI2c(uint8_t bits);

    int fd_ = -1;
    uint8_t backlight_ = 0x08;  // P3 high = backlight on
};

}  // namespace rtsynth
