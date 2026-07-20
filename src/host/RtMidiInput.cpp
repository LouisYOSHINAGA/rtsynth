#include <iostream>
#include "RtMidiInput.hpp"

namespace rtsynth {

bool RtMidiInput::ensureProbe(){
    if(probe_ != nullptr){
        return true;
    }
    try{
        probe_ = std::make_unique<RtMidiIn>();
        return true;
    }catch(const RtMidiError& e){
        std::cerr << "MIDI is unavailable: " << e.getMessage() << std::endl;
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

bool RtMidiInput::openOne(unsigned int index, const std::string& name){
    auto port = std::make_unique<Port>();
    port->owner = this;
    port->name = name;
    try{
        port->midi = std::make_unique<RtMidiIn>();
        port->midi->openPort(index);
    }catch(const RtMidiError& e){
        std::cerr << "Failed to open MIDI port [" << index << "] " << name
                  << ": " << e.getMessage() << std::endl;
        return false;
    }
    port->midi->ignoreTypes(true, true, true);  // sysex, timing, active sensing
    port->midi->setCallback(&rtCallback, port.get());
    ports_.push_back(std::move(port));
    return true;
}

bool RtMidiInput::open(int portIndex){
    if(!ensureProbe()){
        return false;
    }
    const unsigned int count = probe_->getPortCount();
    if(count == 0){
        std::cerr << "No MIDI input port found." << std::endl;
        return false;
    }

    if(portIndex >= 0){  // explicit single port
        if(static_cast<unsigned int>(portIndex) >= count){
            std::cerr << "MIDI port " << portIndex << " does not exist." << std::endl;
            return false;
        }
        return openOne(static_cast<unsigned int>(portIndex),
                       probe_->getPortName(static_cast<unsigned int>(portIndex)));
    }

    // default: every real device, so notes and CC can come from different
    // hardware at the same time ("Midi Through" would only loop us back)
    for(unsigned int i = 0; i < count; i++){
        const std::string name = probe_->getPortName(i);
        if(name.find("Midi Through") != std::string::npos){
            continue;
        }
        openOne(i, name);  // a single failing port shouldn't stop the rest
    }

    if(ports_.empty()){
        std::cerr << "No usable MIDI input port found (only Midi Through)." << std::endl;
        return false;
    }
    return true;
}

void RtMidiInput::close(){
    for(auto& port : ports_){
        if(port->midi != nullptr && port->midi->isPortOpen()){
            port->midi->cancelCallback();
            port->midi->closePort();
        }
    }
    ports_.clear();
}

std::string RtMidiInput::openedPortNames() const {
    std::string names;
    for(const auto& port : ports_){
        if(!names.empty()){
            names += ", ";
        }
        names += port->name;
    }
    return names;
}

void RtMidiInput::rtCallback(double /*timestamp*/, std::vector<uint8_t>* message, void* userData){
    auto* port = static_cast<Port*>(userData);
    if(message == nullptr || message->empty()){
        return;
    }

    MidiEvent event;
    if(!MidiEvent::fromRaw(message->data(), message->size(), event)){
        return;  // message type we don't handle
    }

    port->owner->received_.fetch_add(1, std::memory_order_relaxed);
    if(!port->queue.push(event)){
        // never spin/block in the callback: drop and count instead
        port->owner->dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace rtsynth
