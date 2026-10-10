#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include <iostream>

#include "I2cBus.hpp"

namespace rtsynth {

bool I2cBus::open(const std::string& busPath){
    close();
    fd_ = ::open(busPath.c_str(), O_RDWR);
    if(fd_ < 0){
        std::cerr << "Failed to open I2C bus " << busPath
                  << " (is I2C enabled? see: sudo raspi-config)" << std::endl;
        return false;
    }
    return true;
}

void I2cBus::close(){
    if(fd_ >= 0){
        ::close(fd_);
        fd_ = -1;
    }
}

bool I2cBus::writeRegisters(uint8_t address, uint8_t reg, const uint8_t* data, size_t count){
    if(fd_ < 0 || count > 31){
        return false;
    }
    uint8_t buffer[32];
    buffer[0] = reg;
    for(size_t i = 0; i < count; i++){
        buffer[i + 1] = data[i];
    }
    i2c_msg message{};
    message.addr = address;
    message.flags = 0;
    message.len = static_cast<uint16_t>(count + 1);
    message.buf = buffer;
    i2c_rdwr_ioctl_data transfer{&message, 1};
    return ioctl(fd_, I2C_RDWR, &transfer) == 1;
}

bool I2cBus::readRegisters(uint8_t address, uint8_t reg, uint8_t* out, size_t count){
    if(fd_ < 0){
        return false;
    }
    i2c_msg messages[2]{};
    messages[0].addr = address;
    messages[0].flags = 0;
    messages[0].len = 1;
    messages[0].buf = &reg;
    messages[1].addr = address;
    messages[1].flags = I2C_M_RD;
    messages[1].len = static_cast<uint16_t>(count);
    messages[1].buf = out;
    i2c_rdwr_ioctl_data transfer{messages, 2};
    return ioctl(fd_, I2C_RDWR, &transfer) == 2;
}

}  // namespace rtsynth
