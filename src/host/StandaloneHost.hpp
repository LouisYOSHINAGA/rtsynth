#pragma once

#include <atomic>
#include <string>
#include "../core/Processor.hpp"
#include "RtAudioOutput.hpp"
#include "RtMidiInput.hpp"

namespace rtsynth {

// Glue between the platform layer (RtAudio/RtMidi) and a Processor — the
// standalone equivalent of a DAW hosting a plugin. Three threads exist at
// runtime, and this class is where they meet:
//
//   MIDI thread (RtMidi callback)
//       decodes raw bytes -> MidiEvent -> lock-free SpscRingBuffer
//                                              |
//   audio RT thread (RtAudio callback)         v
//       drains the ring into a MidiBuffer, calls Processor::process(),
//       which writes into the driver's planar float buffers
//
//   main thread
//       starts/stops everything, adjusts Parameters (atomic), and polls
//       the xrun / drop counters for reporting — RT threads never log
//
// Swapping RtAudio/RtMidi for another backend (JACK, PipeWire, a plugin
// wrapper, ...) means reimplementing this layer only; core/dsp/synth stay
// untouched.
class StandaloneHost {
public:
    struct Options {
        unsigned int audioDeviceId = RtAudioOutput::kUseDefaultDevice;
        std::string audioApiName;         // "" = auto (prefer direct ALSA)
        int midiPortIndex = -1;           // -1 = connect to all ports
        unsigned int sampleRate = 44100;
        unsigned int bufferFrames = 256;
        unsigned int channels = 2;
        bool requireMidi = true;
    };

    explicit StandaloneHost(Processor& processor) : processor_(processor){}
    ~StandaloneHost(){ stop(); }

    bool start(const Options& options);
    void stop();

    RtAudioOutput& audio(){ return audio_; }
    RtMidiInput& midi(){ return midi_; }

    // events lost because more arrived in one block than MidiBuffer holds
    uint64_t midiOverflowCount() const { return midiOverflow_.load(std::memory_order_relaxed); }

private:
    Processor& processor_;
    RtAudioOutput audio_;
    RtMidiInput midi_;
    MidiBuffer midiBuffer_;
    std::atomic<uint64_t> midiOverflow_{0};
};

}  // namespace rtsynth
