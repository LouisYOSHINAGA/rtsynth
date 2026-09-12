#include <iostream>
#include "StandaloneHost.hpp"

namespace rtsynth {

bool StandaloneHost::start(const Options& options){
    audio_.setVerboseWarnings(options.verboseWarnings);

    // Pick the MIDI backend: raw kernel devices when requested, otherwise
    // the ALSA sequencer via RtMidi. Opening nothing is reported, never
    // fatal — a hardware synth is powered on with whatever happens to be
    // plugged in, and the keyboard may only be connected afterwards. The
    // backend keeps scanning and connects when one appears (rescanMidi).
    if(!options.rawMidiDevices.empty()){
        activeMidi_ = &rawMidi_;
        rawMidi_.open(options.rawMidiDevices);
    }else{
        activeMidi_ = &seqMidi_;
        seqMidi_.open(options.midiPortIndex);
    }
    activeMidi_->setMonitorEnabled(options.monitorMidi);
    std::cout << "MIDI input: " << activeMidi_->description() << std::endl;

    const bool opened = audio_.open(
        options.audioDeviceId, options.sampleRate, options.bufferFrames,
        options.channels,
        [this](AudioBufferView& output){
            // Panel commands first, so a CC or knob move in the same block
            // still lands on top of a revert rather than under it.
            HostCommand command;
            while(commands_.pop(command)){
                if(command == HostCommand::RevertPreset){
                    if(PresetBank* bank = processor_.presets()){
                        bank->revertCurrent();
                    }
                }
            }

            midiBuffer_.clear();
            MidiEvent event;
            // Stop draining when the block buffer is full: the remaining
            // events STAY in the lock-free queues and are processed next
            // block (~ a few ms later). Never pop-then-discard — a burst
            // after an audio stall could otherwise eat note-offs, leaving
            // notes hanging forever.
            while(!midiBuffer_.full() && activeMidi_->pop(event)){
                midiBuffer_.add(event);  // offset 0: applied at block start
            }
            // panel buttons feed the same buffer, so a preset switch from
            // a button and one from a Program Change are the same event
            // on the same thread
            while(!midiBuffer_.full() && controlEvents_.pop(event)){
                midiBuffer_.add(event);
            }
            if(midiBuffer_.full()){
                midiOverflow_.fetch_add(1, std::memory_order_relaxed);
            }

            processor_.process(output, midiBuffer_);
        });

    if(!opened){
        activeMidi_->close();
        return false;
    }

    // prepare with the block size the driver actually chose, then start
    processor_.prepare(static_cast<double>(options.sampleRate),
                       static_cast<int>(audio_.actualBufferFrames()));

    if(!audio_.start()){
        activeMidi_->close();
        return false;
    }

    std::cout << "Audio stream started: " << audio_.currentApiName() << ", "
              << audio_.openedDeviceName() << ", "
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
