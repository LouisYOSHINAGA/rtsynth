#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rtsynth {

// Register-level access to devices on one Linux I2C bus (/dev/i2c-N).
//
// Unlike I2cLcd1602, which binds its fd to one address with I2C_SLAVE,
// every transfer here names its target address (I2C_RDWR), so one open
// bus serves any number of chips — the MCP23017s of a panel all share it.
// A register read is a single combined write-register/repeated-start/read
// transaction, so another process (or the LCD) using the same bus can
// never slip in between the two halves.
class I2cBus {
public:
    ~I2cBus(){ close(); }

    bool open(const std::string& busPath);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    // Writes `count` bytes starting at `reg` (the chip auto-increments).
    bool writeRegisters(uint8_t address, uint8_t reg, const uint8_t* data, size_t count);
    bool writeRegister(uint8_t address, uint8_t reg, uint8_t value){
        return writeRegisters(address, reg, &value, 1);
    }

    // Reads `count` bytes starting at `reg`.
    bool readRegisters(uint8_t address, uint8_t reg, uint8_t* out, size_t count);

private:
    int fd_ = -1;
};

}  // namespace rtsynth
