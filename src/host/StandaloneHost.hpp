#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "../core/Processor.hpp"
#include "MidiInput.hpp"
#include "RawMidiInput.hpp"
#include "RtAudioOutput.hpp"
#include "RtMidiInput.hpp"

namespace rtsynth {

// Glue between the platform layer (RtAudio + a MidiInput backend) and a
// Processor — the standalone equivalent of a DAW hosting a plugin. Three
// threads exist at runtime, and this class is where they meet:
//
//   MIDI thread(s) (RtMidi callback or rawmidi reader)
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
        int midiPortIndex = -1;           // sequencer backend: -1 = all ports
        // non-empty selects the raw kernel-rawmidi backend instead of the
        // ALSA sequencer (see MidiInput.hpp for the trade-off)
        std::vector<std::string> rawMidiDevices;
        unsigned int sampleRate = 44100;
        unsigned int bufferFrames = 256;
        unsigned int channels = 2;
        bool requireMidi = true;
        // debug: show backend warnings and mirror received MIDI into the
        // monitor queues (printed by the main thread, see MidiInput)
        bool verbose = false;
    };

    explicit StandaloneHost(Processor& processor) : processor_(processor){}
    ~StandaloneHost(){ stop(); }

    bool start(const Options& options);
    void stop();

    RtAudioOutput& audio(){ return audio_; }
    MidiInput& midi(){ return *activeMidi_; }

    // times a block's MidiBuffer filled up and the remaining events were
    // deferred to the next block (nothing is lost; high values mean the
    // audio callback is stalling or a controller is flooding CCs)
    uint64_t midiDeferralCount() const { return midiOverflow_.load(std::memory_order_relaxed); }

private:
    Processor& processor_;
    RtAudioOutput audio_;
    RtMidiInput seqMidi_;
    RawMidiInput rawMidi_;
    MidiInput* activeMidi_ = &seqMidi_;
    MidiBuffer midiBuffer_;
    std::atomic<uint64_t> midiOverflow_{0};
};

}  // namespace rtsynth
