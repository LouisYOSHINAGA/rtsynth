#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

#include <iostream>
#include "Mcp3008Input.hpp"

namespace rtsynth {

bool Mcp3008Input::open(const std::string& devicePath, uint32_t speedHz){
    close();

    fd_ = ::open(devicePath.c_str(), O_RDWR);
    if(fd_ < 0){
        std::cerr << "Failed to open SPI device " << devicePath
                  << " (is SPI enabled? see: sudo raspi-config)" << std::endl;
        return false;
    }

    speedHz_ = speedHz;
    uint8_t mode = SPI_MODE_0;
    uint8_t bitsPerWord = 8;
    if(ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0
    || ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bitsPerWord) < 0
    || ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speedHz_) < 0){
        std::cerr << "Failed to configure SPI device " << devicePath << std::endl;
        close();
        return false;
    }
    return true;
}

void Mcp3008Input::close(){
    if(fd_ >= 0){
        ::close(fd_);
        fd_ = -1;
    }
}

bool Mcp3008Input::read(int channel, float& normalizedOut){
    if(fd_ < 0 || channel < 0 || channel >= numChannels()){
        return false;
    }

    // MCP3008 frame (datasheet 6.1): start bit, then single-ended mode +
    // channel select, then the device clocks out the 10-bit result
    uint8_t tx[3] = {0x01, static_cast<uint8_t>((0x08 | channel) << 4), 0x00};
    uint8_t rx[3] = {0, 0, 0};

    spi_ioc_transfer transfer{};
    transfer.tx_buf = reinterpret_cast<uintptr_t>(tx);
    transfer.rx_buf = reinterpret_cast<uintptr_t>(rx);
    transfer.len = 3;
    transfer.speed_hz = speedHz_;
    transfer.bits_per_word = 8;

    if(ioctl(fd_, SPI_IOC_MESSAGE(1), &transfer) < 0){
        return false;
    }

    const int value = ((rx[1] & 0x03) << 8) | rx[2];
    normalizedOut = static_cast<float>(value) / 1023.0f;
    return true;
}

}  // namespace rtsynth
