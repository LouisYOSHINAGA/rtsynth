#include <errno.h>
#include <poll.h>

#include <algorithm>
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

size_t RawMidiInput::connectedDevices() const {
    size_t connected = 0;
    const size_t slots = deviceCount_.load(std::memory_order_relaxed);
    for(size_t i = 0; i < slots; i++){
        if(devices_[i]->connected){
            connected++;
        }
    }
    return connected;
}

bool RawMidiInput::connectDevice(const std::string& id, const std::string& name, bool initial){
    const size_t slots = deviceCount_.load(std::memory_order_relaxed);
    Device* device = nullptr;
    for(size_t i = 0; i < slots && device == nullptr; i++){
        if(!devices_[i]->connected && devices_[i]->id == id){
            device = devices_[i].get();  // same device back again
        }
    }
    for(size_t i = 0; i < slots && device == nullptr; i++){
        if(!devices_[i]->connected){
            device = devices_[i].get();
        }
    }

    const bool freshSlot = (device == nullptr);
    std::unique_ptr<Device> created;
    if(freshSlot){
        if(slots >= kMaxDevices){
            if(!slotsExhausted_){
                slotsExhausted_ = true;
                std::cerr << "[warning] more than " << kMaxDevices
                          << " raw MIDI devices; ignoring " << id << std::endl;
            }
            return false;
        }
        created = std::make_unique<Device>();
        device = created.get();
    }

    const int err = snd_rawmidi_open(&device->in, nullptr, id.c_str(),
                                     SND_RAWMIDI_NONBLOCK);
    if(err < 0){
        device->in = nullptr;
        // a rescan runs once a second, so only the startup attempt speaks
        if(initial){
            std::cerr << "Failed to open raw MIDI device " << id << ": "
                      << snd_strerror(err) << " (see --list for device ids)" << std::endl;
        }
        return false;
    }

    device->owner = this;
    device->id = id;
    device->name = name.empty()? id : name;
    device->parser.reset();       // a reconnect starts a fresh byte stream
    device->gone.store(false, std::memory_order_relaxed);
    device->connected = true;

    if(freshSlot){
        // publish only once the slot is fully built: the audio thread
        // reads devices_[0, deviceCount_) with no other synchronization
        devices_[slots] = std::move(created);
        deviceCount_.store(slots + 1, std::memory_order_release);
    }

    // capture the heap object by pointer — capturing a local by reference
    // would dangle as soon as this function returns
    Device* raw = device;
    device->thread = std::thread([this, raw]{ readerThread(*raw); });
    return true;
}

void RawMidiInput::reapDevice(Device& device){
    if(device.thread.joinable()){
        device.thread.join();
    }
    if(device.in != nullptr){
        snd_rawmidi_close(device.in);
        device.in = nullptr;
    }
    device.connected = false;

    // The reader thread is joined, so the main thread is now this queue's
    // only producer. An unplugged keyboard cannot send the note-offs for
    // whatever it was holding, so release them here.
    for(uint8_t channel = 0; channel < 16; channel++){
        device.queue.push(MidiEvent::controlChange(channel, 123, 0));  // all notes off
    }
}

bool RawMidiInput::scan(bool initial){
    bool changed = false;
    const size_t slots = deviceCount_.load(std::memory_order_relaxed);

    // 1. reap readers whose device disappeared
    for(size_t i = 0; i < slots; i++){
        Device& device = *devices_[i];
        if(device.connected && device.gone.load(std::memory_order_relaxed)){
            reapDevice(device);
            changed = true;
        }
    }

    const auto inputs = listInputs();
    auto alreadyConnected = [this, slots](const std::string& id){
        for(size_t i = 0; i < slots; i++){
            if(devices_[i]->connected && devices_[i]->id == id){
                return true;
            }
        }
        return false;
    };

    // 2. open what is present and not yet connected
    for(const auto& [id, name] : inputs){
        const bool wanted = wantAll_
                         || std::find(requested_.begin(), requested_.end(), id)
                            != requested_.end();
        if(!wanted || alreadyConnected(id)){
            continue;
        }
        if(connectDevice(id, name, initial)){
            changed = true;
        }
    }
    return changed;
}

bool RawMidiInput::open(const std::vector<std::string>& deviceIds){
    requested_ = deviceIds;
    wantAll_ = std::find(requested_.begin(), requested_.end(), "all") != requested_.end();
    running_.store(true);
    scan(true);
    // Nothing connected is not an error: rescan() picks devices up as
    // they are plugged in.
    return connectedDevices() > 0;
}

void RawMidiInput::close(){
    running_.store(false);
    // called with the audio stream already stopped, so the slots may go
    const size_t slots = deviceCount_.load(std::memory_order_relaxed);
    deviceCount_.store(0, std::memory_order_release);
    for(size_t i = 0; i < slots; i++){
        Device& device = *devices_[i];
        if(device.thread.joinable()){
            device.thread.join();
        }
        if(device.in != nullptr){
            snd_rawmidi_close(device.in);
            device.in = nullptr;
        }
        device.connected = false;
        devices_[i].reset();
    }
    requested_.clear();
    wantAll_ = false;
    slotsExhausted_ = false;
}

std::string RawMidiInput::description() const {
    const size_t slots = deviceCount_.load(std::memory_order_relaxed);
    const size_t connected = connectedDevices();
    if(connected == 0){
        return "raw MIDI (kernel rawmidi), no device connected yet"
               " — will connect automatically when one appears";
    }

    std::string text = "raw MIDI (kernel rawmidi), "
                     + std::to_string(connected) + " device(s): ";
    bool first = true;
    for(size_t i = 0; i < slots; i++){
        if(!devices_[i]->connected){
            continue;
        }
        if(!first){
            text += ", ";
        }
        text += devices_[i]->id;
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

    while(running_.load() && !device.gone.load(std::memory_order_relaxed)){
        const int ready = ::poll(fds, static_cast<nfds_t>(nfds), 200 /* ms */);
        if(ready < 0){
            if(errno == EINTR){
                continue;
            }
            device.gone.store(true, std::memory_order_relaxed);
            break;
        }
        if(ready == 0){
            continue;  // timeout: re-checks running_ / gone
        }

        // An unplugged device shows up here as an error/hangup rather than
        // as readable data. Hand the slot back to scan() instead of
        // spinning on a descriptor that will never be readable again.
        unsigned short revents = 0;
        if(snd_rawmidi_poll_descriptors_revents(device.in, fds, nfds, &revents) < 0
           || (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0){
            device.gone.store(true, std::memory_order_relaxed);
            break;
        }
        if((revents & POLLIN) == 0){
            continue;
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
        }else if(bytes == -ENODEV || bytes == -ENXIO){
            device.gone.store(true, std::memory_order_relaxed);  // unplugged
            break;
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
