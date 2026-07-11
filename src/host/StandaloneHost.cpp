#include <iostream>
#include "StandaloneHost.hpp"

namespace rtsynth {

bool StandaloneHost::start(const Options& options){
    if(!midi_.open(options.midiPortIndex)){
        if(options.requireMidi){
            return false;
        }
        std::cerr << "Continuing without MIDI input." << std::endl;
    }else{
        std::cout << "MIDI input: " << midi_.openedPortName() << std::endl;
    }

    const bool opened = audio_.open(
        options.audioDeviceId, options.sampleRate, options.bufferFrames,
        options.channels,
        [this](AudioBufferView& output){
            midiBuffer_.clear();
            MidiEvent event;
            while(midi_.pop(event)){
                if(!midiBuffer_.add(event)){  // offset 0: applied at block start
                    midiOverflow_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            processor_.process(output, midiBuffer_);
        });

    if(!opened){
        midi_.close();
        return false;
    }

    // prepare with the block size the driver actually chose, then start
    processor_.prepare(static_cast<double>(options.sampleRate),
                       static_cast<int>(audio_.actualBufferFrames()));

    if(!audio_.start()){
        midi_.close();
        return false;
    }

    std::cout << "Audio stream started: " << options.sampleRate << " Hz, "
              << audio_.actualBufferFrames() << " frames/block, "
              << options.channels << " ch" << std::endl;
    return true;
}

void StandaloneHost::stop(){
    audio_.close();
    midi_.close();
}

}  // namespace rtsynth
