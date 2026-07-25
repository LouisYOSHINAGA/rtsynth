#include <iostream>
#include "StandaloneHost.hpp"

namespace rtsynth {

bool StandaloneHost::start(const Options& options){
    audio_.setVerboseWarnings(options.verbose);

    // pick the MIDI backend: raw kernel devices when requested, otherwise
    // the ALSA sequencer via RtMidi
    bool midiOpen;
    if(!options.rawMidiDevices.empty()){
        activeMidi_ = &rawMidi_;
        midiOpen = rawMidi_.open(options.rawMidiDevices);
    }else{
        activeMidi_ = &seqMidi_;
        midiOpen = seqMidi_.open(options.midiPortIndex);
    }
    activeMidi_->setMonitorEnabled(options.verbose);

    if(!midiOpen){
        if(options.requireMidi){
            return false;
        }
        std::cerr << "Continuing without MIDI input." << std::endl;
    }else{
        std::cout << "MIDI input: " << activeMidi_->description() << std::endl;
    }

    const bool opened = audio_.open(
        options.audioDeviceId, options.sampleRate, options.bufferFrames,
        options.channels,
        [this](AudioBufferView& output){
            midiBuffer_.clear();
            MidiEvent event;
            // Stop draining when the block buffer is full: the remaining
            // events STAY in the lock-free queues and are processed next
            // block (~ a few ms later). Never pop-then-discard — a burst
            // after an audio stall could otherwise eat note-offs, leaving
            // notes hanging forever.
            while(!midiBuffer_.full() && activeMidi_->pop(event)){
                midiBuffer_.add(event);  // offset 0: applied at block start
                noteGuard_.observe(event);
            }
            if(midiBuffer_.full()){
                midiOverflow_.fetch_add(1, std::memory_order_relaxed);
            }

            // recover notes whose note-off the source never sent
            noteGuard_.advance(output.numFrames());
            noteGuard_.collectExpired([this](const MidiEvent& off){
                if(!midiBuffer_.add(off)){
                    return false;
                }
                recoveredNotes_.fetch_add(1, std::memory_order_relaxed);
                return true;
            });

            processor_.process(output, midiBuffer_);
        });

    if(!opened){
        activeMidi_->close();
        return false;
    }

    // prepare with the block size the driver actually chose, then start
    processor_.prepare(static_cast<double>(options.sampleRate),
                       static_cast<int>(audio_.actualBufferFrames()));
    noteGuard_.prepare(static_cast<double>(options.sampleRate),
                       options.noteTimeoutSeconds);

    if(!audio_.start()){
        activeMidi_->close();
        return false;
    }

    std::cout << "Audio stream started: " << audio_.currentApiName() << ", "
              << options.sampleRate << " Hz, "
              << audio_.actualBufferFrames() << " frames/block, "
              << options.channels << " ch" << std::endl;
    return true;
}

void StandaloneHost::stop(){
    audio_.close();
    seqMidi_.close();
    rawMidi_.close();
}

}  // namespace rtsynth
