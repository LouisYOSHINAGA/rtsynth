#include <errno.h>
#include <poll.h>

#include <cstdio>
#include <iostream>

#include "RawMidiInput.hpp"

namespace rtsynth {

std::vector<std::pair<std::string, std::string>> RawMidiInput::listInputs(){
    std::vector<std::pair<std::string, std::string>> inputs;

    int card = -1;
    while(snd_card_next(&card) >= 0 && card >= 0){
        char ctlId[32];
        std::snprintf(ctlId, sizeof(ctlId), "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        if(snd_ctl_open(&ctl, ctlId, 0) < 0){
            continue;
        }

        int device = -1;
        while(snd_ctl_rawmidi_next_device(ctl, &device) >= 0 && device >= 0){
            snd_rawmidi_info_t* info = nullptr;
            snd_rawmidi_info_alloca(&info);
            snd_rawmidi_info_set_device(info, static_cast<unsigned int>(device));
            snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
            snd_rawmidi_info_set_subdevice(info, 0);
            if(snd_ctl_rawmidi_info(ctl, info) < 0){
                continue;  // no input stream on this device
            }

            const unsigned int subdevices = snd_rawmidi_info_get_subdevices_count(info);
            for(unsigned int sub = 0; sub < subdevices; sub++){
                snd_rawmidi_info_set_subdevice(info, sub);
                if(snd_ctl_rawmidi_info(ctl, info) < 0){
                    continue;
                }
                char id[32];
                std::snprintf(id, sizeof(id), "hw:%d,%d,%u", card, device, sub);
                const char* subName = snd_rawmidi_info_get_subdevice_name(info);
                const std::string name = (subName != nullptr && subName[0] != '\0')
                                       ? subName : snd_rawmidi_info_get_name(info);
                inputs.emplace_back(id, name);
            }
        }
        snd_ctl_close(ctl);
    }
    return inputs;
}

bool RawMidiInput::open(const std::vector<std::string>& deviceIds){
    for(const std::string& id : deviceIds){
        auto device = std::make_unique<Device>();
        device->owner = this;
        device->id = id;
        device->name = id;

        const int err = snd_rawmidi_open(&device->in, nullptr, id.c_str(),
                                         SND_RAWMIDI_NONBLOCK);
        if(err < 0){
            std::cerr << "Failed to open raw MIDI device " << id << ": "
                      << snd_strerror(err) << " (see --list for device ids)" << std::endl;
            continue;  // one bad device shouldn't stop the rest
        }
        devices_.push_back(std::move(device));
    }

    if(devices_.empty()){
        std::cerr << "No raw MIDI device could be opened." << std::endl;
        return false;
    }

    running_.store(true);
    for(auto& device : devices_){
        // capture the heap object by pointer — capturing the loop variable
        // by reference would dangle once the loop advances/exits
        Device* raw = device.get();
        device->thread = std::thread([this, raw]{ readerThread(*raw); });
    }
    return true;
}

void RawMidiInput::close(){
    running_.store(false);
    for(auto& device : devices_){
        if(device->thread.joinable()){
            device->thread.join();
        }
        if(device->in != nullptr){
            snd_rawmidi_close(device->in);
            device->in = nullptr;
        }
    }
    devices_.clear();
}

std::string RawMidiInput::description() const {
    std::string text = "raw MIDI (kernel rawmidi), "
                     + std::to_string(devices_.size()) + " device(s): ";
    bool first = true;
    for(const auto& device : devices_){
        if(!first){
            text += ", ";
        }
        text += device->id;
        first = false;
    }
    return text;
}

void RawMidiInput::readerThread(Device& device){
    pollfd fds[4];
    const int nfds = snd_rawmidi_poll_descriptors(device.in, fds, 4);

    auto emit = [this, &device](const MidiEvent& event){
        received_.fetch_add(1, std::memory_order_relaxed);
        if(!device.queue.push(event)){
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        if(monitor_.load(std::memory_order_relaxed)){
            device.monitorQueue.push(event);  // best effort
        }
    };

    while(running_.load()){
        const int ready = ::poll(fds, static_cast<nfds_t>(nfds), 200 /* ms */);
        if(ready <= 0){
            continue;  // timeout (re-checks running_) or transient error
        }

        uint8_t buffer[512];
        const ssize_t bytes = snd_rawmidi_read(device.in, buffer, sizeof(buffer));
        if(bytes > 0){
            if(rawDump_.load(std::memory_order_relaxed)){
                for(ssize_t i = 0; i < bytes; i++){
                    device.rawQueue.push({buffer[i], i == 0});  // best effort
                }
            }
            device.parser.feed(buffer, static_cast<size_t>(bytes), emit);
        }else if(bytes < 0 && bytes != -EAGAIN && bytes != -EINTR){
            // -EPIPE means the kernel's rawmidi input buffer overran and
            // bytes were discarded by the driver: exactly the kind of
            // silent loss that leaves notes hanging. Count it, then
            // resynchronize the parser since the stream may be truncated
            // mid-message.
            readErrors_.fetch_add(1, std::memory_order_relaxed);
            device.parser.reset();
            if(bytes == -EPIPE){
                snd_rawmidi_drop(device.in);
            }
        }
    }
}

}  // namespace rtsynth
