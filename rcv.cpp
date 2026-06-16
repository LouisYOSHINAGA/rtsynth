#include <iostream>
#include <vector>
#include <unistd.h>
#include <RtMidi.h>
#include "gen.hpp"


void midi_callback(double timestamp, std::vector<uint8_t> *message, void* userData){
    if(message->size() < 3){
        std::cout << "Invalid message." << std::endl;
        return;
    }

    SynthState* synth_state = static_cast<SynthState*>(userData);
    NoteEvent note_event;

    uint8_t status = (*message)[0] & 0xF0;
    uint8_t noteno = (*message)[1];
    uint8_t velocity = (*message)[2];

    // note on
    if(status == 0x90 && velocity > 0){
        note_event = {.note=noteno, .is_note_on=true};

        std::cout << "note on: note = " << +noteno << "; "
                  << "velocity = " << +velocity
                  << std::endl;
    }
    // note off
    else if(status == 0x80 || (status == 0x90 && velocity == 0)){
        note_event = {.note=noteno, .is_note_on=false};

        std::cout << "note off: note = " << +noteno << std::endl;
    }

    while(!synth_state->voice_queue.push(note_event));
}


void print_ports(RtMidi* midi_in){
    uint8_t num_port = midi_in->getPortCount();

    if(num_port == 0){
        std::cout << "No MIDI ports found." << std::endl;
        return;
    }

    std::cout << "MIDI ports found: " << num_port << std::endl;
    for(uint8_t i = 0; i < num_port; i++){
        std::cout << "[" << +i << "] " << midi_in->getPortName(i) << std::endl;
    }
}


void connect_port(RtMidiIn* midi_in, SynthState* state){
    for(uint8_t i = 0; i < midi_in->getPortCount(); i++){
        if(midi_in->getPortName(i).find("Midi Through") == std::string::npos){
            midi_in->openPort(i);
            midi_in->setCallback(midi_callback, state);
            midi_in->ignoreTypes(true, true, true);  // sysex, timing, active sensing
            std::cout << "Connected port " << +i << ": " << midi_in->getPortName(i) << std::endl;
            break;
        }
    }
}


int main(){
    AudioEngine audio_engine;
    audio_engine.start();

    RtMidiIn *midi_in = new RtMidiIn();
    print_ports(midi_in);
    connect_port(midi_in, &audio_engine.get_state());

    std::cout << "Waiting for MIDI input ..." << std::endl;
    while(true){
        sleep(1);
    }
}