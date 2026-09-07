#include <algorithm>
#include <iostream>
#include "RtMidiInput.hpp"

namespace rtsynth {

namespace {

bool isThroughPort(const std::string& name){
    return name.find("Midi Through") != std::string::npos;
}

}  // namespace

bool RtMidiInput::ensureProbe(){
    if(probe_ != nullptr){
        return true;
    }
    try{
        probe_ = std::make_unique<RtMidiIn>();
        probeErrorReported_ = false;
        return true;
    }catch(const RtMidiError& e){
        // rescan() retries every tick, so this must be said once, not once
        // per second (the sequencer can appear later than we do at boot)
        if(!probeErrorReported_){
            probeErrorReported_ = true;
            std::cerr << "MIDI is unavailable: " << e.getMessage() << std::endl;
        }
        return false;
    }
}

std::vector<std::string> RtMidiInput::listPorts(){
    std::vector<std::string> ports;
    if(!ensureProbe()){
        return ports;
    }
    const unsigned int count = probe_->getPortCount();
    for(unsigned int i = 0; i < count; i++){
        ports.push_back(probe_->getPortName(i));
    }
    return ports;
}

size_t RtMidiInput::connectedPorts() const {
    size_t connected = 0;
    const size_t slots = portCount_.load(std::memory_order_relaxed);
    for(size_t i = 0; i < slots; i++){
        if(ports_[i]->connected){
            connected++;
        }
    }
    return connected;
}

bool RtMidiInput::connectPort(unsigned int index, const std::string& name, bool initial){
    // Reuse a slot left behind by a device that went away — the sequencer
    // renames a port on every replug (the client number is part of the
    // name), so without recycling, unplugging the same keyboard a dozen
    // times would exhaust the slots.
    const size_t slots = portCount_.load(std::memory_order_relaxed);
    Port* port = nullptr;
    for(size_t i = 0; i < slots && port == nullptr; i++){
        if(!ports_[i]->connected && ports_[i]->name == name){
            port = ports_[i].get();  // same device back again
        }
    }
    for(size_t i = 0; i < slots && port == nullptr; i++){
        if(!ports_[i]->connected){
            port = ports_[i].get();
        }
    }

    const bool freshSlot = (port == nullptr);
    std::unique_ptr<Port> created;
    if(freshSlot){
        if(slots >= kMaxPorts){
            if(!slotsExhausted_){
                slotsExhausted_ = true;
                std::cerr << "[warning] more than " << kMaxPorts
                          << " MIDI ports; ignoring " << name << std::endl;
            }
            return false;
        }
        created = std::make_unique<Port>();
        port = created.get();
    }

    port->owner = this;
    port->name = name;
    try{
        port->midi = std::make_unique<RtMidiIn>();
        // configure BEFORE opening: RtMidi routes messages that arrive with
        // no callback attached into an internal queue where they are lost
        // to us, so attaching the callback after openPort() drops events
        port->midi->ignoreTypes(true, true, true);  // sysex, timing, active sensing
        port->midi->setCallback(&rtCallback, port);
        port->midi->openPort(index);
    }catch(const RtMidiError& e){
        port->midi.reset();
        // A rescan runs once a second and races with devices settling, so
        // only the startup attempt is worth a message.
        if(initial){
            std::cerr << "Failed to open MIDI port [" << index << "] " << name
                      << ": " << e.getMessage() << std::endl;
        }
        return false;
    }
    // Indices are only valid for the instant they were read: a device
    // appearing between the enumeration and this openPort() would shift
    // them and connect us to a port under the wrong name, which no later
    // scan would notice. Confirm before recording it.
    if(probe_->getPortName(index) != name){
        try{
            port->midi->cancelCallback();
            port->midi->closePort();
        }catch(const RtMidiError&){
        }
        port->midi.reset();
        return false;  // the next scan sees a settled port list
    }
    port->connected = true;

    if(freshSlot){
        // publish only once the port is fully built: the audio thread
        // reads ports_[0, portCount_) with no other synchronization
        ports_[slots] = std::move(created);
        portCount_.store(slots + 1, std::memory_order_release);
    }
    return true;
}

void RtMidiInput::disconnectPort(Port& port){
    if(port.midi != nullptr){
        try{
            port.midi->cancelCallback();
            port.midi->closePort();  // joins RtMidi's input thread
        }catch(const RtMidiError&){
            // the port is already gone; nothing left to release
        }
        port.midi.reset();
    }
    port.connected = false;

    // The callback thread is joined, so the main thread is now this
    // queue's only producer. An unplugged keyboard cannot send the
    // note-offs for whatever it was holding, so release them here — the
    // alternative is a note that sounds until the process is killed.
    for(uint8_t channel = 0; channel < 16; channel++){
        port.queue.push(MidiEvent::controlChange(channel, 123, 0));  // all notes off
    }
}

bool RtMidiInput::scan(bool initial){
    if(!ensureProbe()){
        return false;
    }

    std::vector<std::string> current;
    try{
        const unsigned int count = probe_->getPortCount();
        for(unsigned int i = 0; i < count; i++){
            current.push_back(probe_->getPortName(i));
        }
    }catch(const RtMidiError&){
        // the sequencer changed under the enumeration; try again next tick
        return false;
    }
    discovered_ = current;

    bool changed = false;
    const size_t slots = portCount_.load(std::memory_order_relaxed);

    // 1. release ports that disappeared
    for(size_t i = 0; i < slots; i++){
        Port& port = *ports_[i];
        if(port.connected
           && std::find(current.begin(), current.end(), port.name) == current.end()){
            disconnectPort(port);
            changed = true;
        }
    }

    auto alreadyConnected = [this, slots](const std::string& name){
        for(size_t i = 0; i < slots; i++){
            if(ports_[i]->connected && ports_[i]->name == name){
                return true;
            }
        }
        return false;
    };

    // 2. connect what appeared
    if(portIndex_ >= 0){
        // An index is only meaningful at the moment it is given: the
        // sequencer renumbers as devices come and go. Resolve it to a name
        // once, then follow that name across replugs.
        std::string target = wantedName_;
        if(target.empty() && static_cast<size_t>(portIndex_) < current.size()){
            target = current[static_cast<size_t>(portIndex_)];
        }
        const auto it = std::find(current.begin(), current.end(), target);
        if(!target.empty() && it != current.end() && !alreadyConnected(target)){
            const auto index = static_cast<unsigned int>(std::distance(current.begin(), it));
            if(connectPort(index, target, initial)){
                wantedName_ = target;
                changed = true;
            }
        }
    }else{
        // every real device, so notes and CC can come from different
        // hardware at the same time ("Midi Through" would only loop back)
        for(size_t i = 0; i < current.size(); i++){
            if(isThroughPort(current[i]) || alreadyConnected(current[i])){
                continue;
            }
            if(connectPort(static_cast<unsigned int>(i), current[i], initial)){
                changed = true;  // a failing port shouldn't stop the rest
            }
        }
    }
    return changed;
}

bool RtMidiInput::open(int portIndex){
    portIndex_ = portIndex;
    scan(true);
    // Nothing connected is not an error: the caller keeps running and
    // rescan() picks the device up when it is plugged in.
    return connectedPorts() > 0;
}

void RtMidiInput::close(){
    // called with the audio stream already stopped, so the slots may go
    const size_t slots = portCount_.load(std::memory_order_relaxed);
    portCount_.store(0, std::memory_order_release);
    for(size_t i = 0; i < slots; i++){
        Port& port = *ports_[i];
        if(port.midi != nullptr){
            try{
                port.midi->cancelCallback();
                port.midi->closePort();
            }catch(const RtMidiError&){
            }
            port.midi.reset();
        }
        port.connected = false;
        ports_[i].reset();
    }
    discovered_.clear();
    wantedName_.clear();
    slotsExhausted_ = false;
}

std::string RtMidiInput::description() const {
    const size_t slots = portCount_.load(std::memory_order_relaxed);
    const size_t connected = connectedPorts();
    if(connected == 0){
        return "ALSA sequencer (RtMidi), no device connected yet"
               " — will connect automatically when one appears";
    }

    std::string text = "ALSA sequencer (RtMidi), connected "
                     + std::to_string(connected) + " of "
                     + std::to_string(std::max(discovered_.size(), connected))
                     + " port(s):";
    for(size_t i = 0; i < slots; i++){
        if(ports_[i]->connected){
            text += "\n  [connected] " + ports_[i]->name;
        }
    }
    for(const std::string& name : discovered_){
        bool opened = false;
        for(size_t i = 0; i < slots && !opened; i++){
            opened = ports_[i]->connected && ports_[i]->name == name;
        }
        if(!opened){
            text += "\n  [skipped]   " + name;
        }
    }
    return text;
}

void RtMidiInput::rtCallback(double /*timestamp*/, std::vector<uint8_t>* message, void* userData){
    auto* port = static_cast<Port*>(userData);
    if(message == nullptr || message->empty()){
        return;
    }

    if(port->owner->rawDump_.load(std::memory_order_relaxed)){
        for(size_t i = 0; i < message->size(); i++){
            port->rawQueue.push({(*message)[i], i == 0});  // best effort
        }
    }

    MidiEvent event;
    if(!MidiEvent::fromRaw(message->data(), message->size(), event)){
        // not necessarily an error (aftertouch etc.), but a growing count
        // alongside missing notes points at malformed messages
        port->owner->undecoded_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    port->owner->received_.fetch_add(1, std::memory_order_relaxed);
    if(!port->queue.push(event)){
        // never spin/block in the callback: drop and count instead
        port->owner->dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    if(port->owner->monitor_.load(std::memory_order_relaxed)){
        port->monitorQueue.push(event);  // best effort; overflow just skips prints
    }
}

}  // namespace rtsynth
