#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

#include <algorithm>
#include <iostream>

#include "ShiftRegisterLeds.hpp"

namespace rtsynth {

std::string ShiftRegisterLeds::pinName(int channel){
    return "sr" + std::to_string(channel / 8) + "." + std::to_string(channel % 8);
}

void ShiftRegisterLeds::setChainLength(int chips){
    bytes_.assign(static_cast<size_t>(std::max(chips, 0)), 0);
    invert_.assign(bytes_.size(), 0);
    dirty_ = true;
}

void ShiftRegisterLeds::setActiveLow(int channel, bool activeLow){
    if(channel < 0 || channel >= numChannels()){
        return;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (channel % 8));
    uint8_t& mask = invert_[static_cast<size_t>(channel / 8)];
    mask = activeLow? (mask | bit) : static_cast<uint8_t>(mask & ~bit);
    dirty_ = true;
}

bool ShiftRegisterLeds::open(const std::string& devicePath, uint32_t speedHz){
    close();
    fd_ = ::open(devicePath.c_str(), O_RDWR);
    if(fd_ < 0){
        std::cerr << "Failed to open SPI device " << devicePath
                  << " (is SPI enabled? see: sudo raspi-config)" << std::endl;
        return false;
    }
    speedHz_ = speedHz;
    uint8_t mode = SPI_MODE_0;        // 595 shifts on the rising SRCLK edge
    uint8_t bitsPerWord = 8;
    if(ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0
    || ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bitsPerWord) < 0
    || ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speedHz_) < 0){
        std::cerr << "Failed to configure SPI device " << devicePath << std::endl;
        close();
        return false;
    }
    dirty_ = true;
    flush();  // every output to its "off" level before anything lights
    return true;
}

void ShiftRegisterLeds::close(){
    if(fd_ >= 0){
        // leave the panel dark rather than frozen in its last state
        std::fill(bytes_.begin(), bytes_.end(), 0);
        dirty_ = true;
        flush();
        ::close(fd_);
        fd_ = -1;
    }
}

void ShiftRegisterLeds::setLed(int channel, bool on){
    if(channel < 0 || channel >= numChannels()){
        return;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (channel % 8));
    uint8_t& byte = bytes_[static_cast<size_t>(channel / 8)];
    const uint8_t next = on? (byte | bit) : static_cast<uint8_t>(byte & ~bit);
    if(next != byte){
        byte = next;
        dirty_ = true;
    }
}

std::vector<uint8_t> ShiftRegisterLeds::frame() const {
    // The first byte shifted in travels furthest down the chain, so the
    // last chip goes first. MSB first: bit 7 lands on QH, bit 0 on QA.
    std::vector<uint8_t> out(bytes_.size());
    for(size_t chip = 0; chip < bytes_.size(); chip++){
        out[bytes_.size() - 1 - chip] = bytes_[chip] ^ invert_[chip];
    }
    return out;
}

void ShiftRegisterLeds::flush(){
    if(fd_ < 0 || !dirty_ || bytes_.empty()){
        return;
    }
    const std::vector<uint8_t> out = frame();
    dirty_ = false;
    if(out == written_){
        return;
    }
    spi_ioc_transfer transfer{};
    transfer.tx_buf = reinterpret_cast<uintptr_t>(out.data());
    transfer.len = static_cast<uint32_t>(out.size());
    transfer.speed_hz = speedHz_;
    transfer.bits_per_word = 8;
    if(ioctl(fd_, SPI_IOC_MESSAGE(1), &transfer) < 0){
        dirty_ = true;  // try again on the next tick
        return;
    }
    written_ = out;
}

}  // namespace rtsynth
