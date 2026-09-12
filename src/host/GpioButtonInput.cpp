#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/gpio.h>

#include <cstring>
#include <iostream>
#include "GpioButtonInput.hpp"

namespace rtsynth {

int GpioButtonInput::addButton(unsigned int pin){
    auto button = std::make_unique<Button>();
    button->pin = pin;
    buttons_.push_back(std::move(button));
    return static_cast<int>(buttons_.size()) - 1;
}

bool GpioButtonInput::open(const std::string& chipPath, unsigned int debounceUs){
    if(buttons_.empty()){
        return false;
    }

    const int chipFd = ::open(chipPath.c_str(), O_RDWR);
    if(chipFd < 0){
        std::cerr << "Failed to open GPIO chip " << chipPath << std::endl;
        return false;
    }

    bool ok = true;
    debounceActive_ = true;
    for(auto& button : buttons_){
        // Ask for kernel-side debounce first. gpiolib implements it with a
        // software timer where the chip itself cannot, so this normally
        // succeeds — but an older kernel rejects the attribute outright,
        // and raw edges are better than no button at all.
        auto requestLine = [&](bool withDebounce) -> int {
            gpio_v2_line_request request{};
            request.offsets[0] = button->pin;
            request.num_lines = 1;
            request.config.flags = GPIO_V2_LINE_FLAG_INPUT
                                 | GPIO_V2_LINE_FLAG_EDGE_RISING
                                 | GPIO_V2_LINE_FLAG_EDGE_FALLING
                                 | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
            if(withDebounce){
                request.config.num_attrs = 1;
                request.config.attrs[0].mask = 0x1;  // applies to line 0
                request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
                request.config.attrs[0].attr.debounce_period_us = debounceUs;
            }
            std::strncpy(request.consumer, "rtsynth", sizeof(request.consumer) - 1);
            if(ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request) < 0){
                return -1;
            }
            return request.fd;
        };

        button->fd = requestLine(true);
        if(button->fd < 0){
            debounceActive_ = false;
            button->fd = requestLine(false);
        }
        if(button->fd < 0){
            std::cerr << "Failed to request GPIO line " << button->pin
                      << " on " << chipPath << std::endl;
            ok = false;
            break;
        }
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

void GpioButtonInput::close(){
    if(running_.exchange(false)){
        if(thread_.joinable()){
            thread_.join();
        }
    }
    for(auto& button : buttons_){
        if(button->fd >= 0){
            ::close(button->fd);
            button->fd = -1;
        }
    }
}

void GpioButtonInput::eventThread(){
    std::vector<pollfd> fds;
    for(const auto& button : buttons_){
        fds.push_back({button->fd, POLLIN, 0});
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

            gpio_v2_line_event events[16];
            const ssize_t bytes = ::read(fds[i].fd, events, sizeof(events));
            const size_t count = (bytes > 0)? bytes / sizeof(events[0]) : 0;

            for(size_t e = 0; e < count; e++){
                // pull-up + switch to GND: falling edge closes the switch
                const bool pressed =
                    (events[e].id == GPIO_V2_LINE_EVENT_FALLING_EDGE);
                if(handler_){
                    handler_(static_cast<int>(i), pressed);
                }
            }
        }
    }
}

}  // namespace rtsynth
