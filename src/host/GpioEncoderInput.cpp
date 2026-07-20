#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/gpio.h>

#include <cstring>
#include <iostream>
#include "GpioEncoderInput.hpp"

namespace rtsynth {

int GpioEncoderInput::addEncoder(unsigned int pinA, unsigned int pinB){
    auto encoder = std::make_unique<Encoder>();
    encoder->pinA = pinA;
    encoder->pinB = pinB;
    encoders_.push_back(std::move(encoder));
    return static_cast<int>(encoders_.size()) - 1;
}

bool GpioEncoderInput::open(const std::string& chipPath){
    if(encoders_.empty()){
        return false;
    }

    const int chipFd = ::open(chipPath.c_str(), O_RDWR);
    if(chipFd < 0){
        std::cerr << "Failed to open GPIO chip " << chipPath << std::endl;
        return false;
    }

    bool ok = true;
    for(auto& encoder : encoders_){
        gpio_v2_line_request request{};
        request.offsets[0] = encoder->pinA;   // values bit 0
        request.offsets[1] = encoder->pinB;   // values bit 1
        request.num_lines = 2;
        request.config.flags = GPIO_V2_LINE_FLAG_INPUT
                             | GPIO_V2_LINE_FLAG_EDGE_RISING
                             | GPIO_V2_LINE_FLAG_EDGE_FALLING
                             | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
        std::strncpy(request.consumer, "rtsynth", sizeof(request.consumer) - 1);

        if(ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request) < 0 || request.fd < 0){
            std::cerr << "Failed to request GPIO lines " << encoder->pinA
                      << "/" << encoder->pinB << " on " << chipPath << std::endl;
            ok = false;
            break;
        }
        encoder->fd = request.fd;

        // adopt the current physical A/B levels as the decoder start state
        gpio_v2_line_values values{};
        values.mask = 0x3;
        if(ioctl(encoder->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) == 0){
            const uint8_t a = static_cast<uint8_t>(values.bits & 0x1);
            const uint8_t b = static_cast<uint8_t>((values.bits >> 1) & 0x1);
            encoder->abState = static_cast<uint8_t>((a << 1) | b);
        }
        encoder->decoder.reset(encoder->abState);
    }
    ::close(chipFd);

    if(!ok){
        close();
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this]{ eventThread(); });
    return true;
}

void GpioEncoderInput::close(){
    if(running_.exchange(false)){
        if(thread_.joinable()){
            thread_.join();
        }
    }
    for(auto& encoder : encoders_){
        if(encoder->fd >= 0){
            ::close(encoder->fd);
            encoder->fd = -1;
        }
    }
}

int GpioEncoderInput::readDelta(int channel){
    if(channel < 0 || channel >= numChannels()){
        return 0;
    }
    return encoders_[static_cast<size_t>(channel)]->steps.exchange(
        0, std::memory_order_relaxed);
}

void GpioEncoderInput::eventThread(){
    std::vector<pollfd> fds;
    for(const auto& encoder : encoders_){
        fds.push_back({encoder->fd, POLLIN, 0});
    }

    while(running_.load()){
        const int ready = ::poll(fds.data(), fds.size(), 200 /* ms */);
        if(ready <= 0){
            continue;  // timeout (checks running_) or transient error
        }

        for(size_t i = 0; i < fds.size(); i++){
            if((fds[i].revents & POLLIN) == 0){
                continue;
            }
            Encoder& encoder = *encoders_[i];

            gpio_v2_line_event events[16];
            const ssize_t bytes = ::read(encoder.fd, events, sizeof(events));
            const size_t count = (bytes > 0)? bytes / sizeof(events[0]) : 0;

            for(size_t e = 0; e < count; e++){
                // update the changed line's bit, then run the decoder
                const uint8_t bit =
                    (events[e].offset == encoder.pinA)? 0x2 : 0x1;  // A = bit1
                if(events[e].id == GPIO_V2_LINE_EVENT_RISING_EDGE){
                    encoder.abState |= bit;
                }else{
                    encoder.abState = static_cast<uint8_t>(encoder.abState & ~bit);
                }
                const int detents = encoder.decoder.onState(encoder.abState);
                if(detents != 0){
                    encoder.steps.fetch_add(detents, std::memory_order_relaxed);
                }
            }
        }
    }
}

}  // namespace rtsynth
