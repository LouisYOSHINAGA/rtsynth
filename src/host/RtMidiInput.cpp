#include <iostream>
#include "RtMidiInput.hpp"

namespace rtsynth {

bool RtMidiInput::ensureCreated(){
    if(midi_ != nullptr){
        return true;
    }
    try{
        midi_ = std::make_unique<RtMidiIn>();
        return true;
    }catch(const RtMidiError& e){
        std::cerr << "MIDI is unavailable: " << e.getMessage() << std::endl;
        return false;
    }
}

std::vector<std::string> RtMidiInput::listPorts(){
    std::vector<std::string> ports;
    if(!ensureCreated()){
        return ports;
    }
    const unsigned int count = midi_->getPortCount();
    for(unsigned int i = 0; i < count; i++){
        ports.push_back(midi_->getPortName(i));
    }
    return ports;
}

bool RtMidiInput::open(int portIndex){
    if(!ensureCreated()){
        return false;
    }
    const unsigned int count = midi_->getPortCount();
    if(count == 0){
        std::cerr << "No MIDI input port found." << std::endl;
        return false;
    }

    unsigned int index = 0;
    if(portIndex >= 0){
        if(static_cast<unsigned int>(portIndex) >= count){
            std::cerr << "MIDI port " << portIndex << " does not exist." << std::endl;
            return false;
        }
        index = static_cast<unsigned int>(portIndex);
    }else{
        bool found = false;
        for(unsigned int i = 0; i < count; i++){
            if(midi_->getPortName(i).find("Midi Through") == std::string::npos){
                index = i;
                found = true;
                break;
            }
        }
        if(!found){
            std::cerr << "No usable MIDI input port found (only Midi Through)." << std::endl;
            return false;
        }
    }

    try{
        midi_->openPort(index);
    }catch(const RtMidiError& e){
        std::cerr << "Failed to open MIDI port: " << e.getMessage() << std::endl;
        return false;
    }

    portName_ = midi_->getPortName(index);
    midi_->ignoreTypes(true, true, true);  // sysex, timing, active sensing
    midi_->setCallback(&rtCallback, this);
    return true;
}

void RtMidiInput::close(){
    if(midi_ != nullptr && midi_->isPortOpen()){
        midi_->cancelCallback();
        midi_->closePort();
    }
}

void RtMidiInput::rtCallback(double /*timestamp*/, std::vector<uint8_t>* message, void* userData){
    auto* self = static_cast<RtMidiInput*>(userData);
    if(message == nullptr || message->empty()){
        return;
    }

    MidiEvent event;
    if(!MidiEvent::fromRaw(message->data(), message->size(), event)){
        return;  // message type we don't handle
    }

    self->received_.fetch_add(1, std::memory_order_relaxed);
    if(!self->queue_.push(event)){
        // never spin/block in the callback: drop and count instead
        self->dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace rtsynth
