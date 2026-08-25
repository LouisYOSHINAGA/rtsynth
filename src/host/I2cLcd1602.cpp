#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/i2c-dev.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "I2cLcd1602.hpp"

namespace rtsynth {

namespace {

// PCF8574 bit assignment on the standard LCD backpack
constexpr uint8_t kBitRs = 0x01;      // register select: 0 = command, 1 = data
constexpr uint8_t kBitEnable = 0x04;  // enable strobe (falling edge latches)

// HD44780 commands (datasheet table 6)
constexpr uint8_t kCmdClear = 0x01;
constexpr uint8_t kCmdEntryMode = 0x06;      // cursor moves right, no shift
constexpr uint8_t kCmdDisplayOn = 0x0C;      // display on, cursor off
constexpr uint8_t kCmdFunction4Bit = 0x28;   // 4-bit, 2 lines, 5x8 font
constexpr uint8_t kCmdSetDdramAddr = 0x80;

void sleepMs(int ms){
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void sleepUs(int us){
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

}  // namespace

bool I2cLcd1602::open(const std::string& busPath, uint8_t address){
    close();

    fd_ = ::open(busPath.c_str(), O_RDWR);
    if(fd_ < 0){
        std::cerr << "Failed to open I2C bus " << busPath
                  << " (is I2C enabled? see: sudo raspi-config)" << std::endl;
        return false;
    }
    if(ioctl(fd_, I2C_SLAVE, address) < 0){
        std::cerr << "Failed to select I2C address 0x" << std::hex << +address
                  << std::dec << " on " << busPath
                  << " (check with: i2cdetect -y 1)" << std::endl;
        close();
        return false;
    }

    // HD44780 4-bit initialization by instruction (datasheet fig. 24):
    // three "function set 8-bit" nibbles with delays, then switch to 4-bit
    sleepMs(50);
    writeNibble(0x03, false); sleepMs(5);
    writeNibble(0x03, false); sleepUs(150);
    writeNibble(0x03, false); sleepUs(150);
    writeNibble(0x02, false); sleepUs(150);   // now in 4-bit mode

    writeByte(kCmdFunction4Bit, false);
    writeByte(kCmdDisplayOn, false);
    writeByte(kCmdEntryMode, false);
    clear();
    return true;
}

void I2cLcd1602::close(){
    if(fd_ >= 0){
        ::close(fd_);
        fd_ = -1;
    }
}

void I2cLcd1602::clear(){
    if(fd_ < 0){
        return;
    }
    writeByte(kCmdClear, false);
    sleepMs(2);  // clear needs >1.52 ms
}

void I2cLcd1602::writeLine(int row, const std::string& text){
    if(fd_ < 0 || row < 0 || row >= rows()){
        return;
    }
    // rows start at DDRAM 0x00 and 0x40 on a 16x2 module
    writeByte(static_cast<uint8_t>(kCmdSetDdramAddr | (row == 0? 0x00 : 0x40)), false);
    for(int col = 0; col < columns(); col++){
        const char c = (col < static_cast<int>(text.size()))? text[static_cast<size_t>(col)] : ' ';
        writeByte(static_cast<uint8_t>(c), true);
    }
}

void I2cLcd1602::writeByte(uint8_t value, bool isData){
    writeNibble(static_cast<uint8_t>(value >> 4), isData);
    writeNibble(static_cast<uint8_t>(value & 0x0F), isData);
    sleepUs(50);  // most commands complete in 37 us
}

void I2cLcd1602::writeNibble(uint8_t nibble, bool isData){
    const uint8_t bits = static_cast<uint8_t>((nibble << 4) | backlight_
                                              | (isData? kBitRs : 0));
    // EN high, then low: the falling edge latches the nibble
    writeI2c(bits | kBitEnable);
    sleepUs(1);
    writeI2c(bits);
    sleepUs(50);
}

void I2cLcd1602::writeI2c(uint8_t bits){
    if(::write(fd_, &bits, 1) != 1){
        // transient bus error; nothing useful to do from the UI thread
    }
}

}  // namespace rtsynth
