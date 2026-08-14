#pragma once

#include <string>

#include "AudioBuffer.hpp"
#include "MidiBuffer.hpp"
#include "Parameters.hpp"
#include "PresetBank.hpp"

namespace rtsynth {

// Platform-independent audio processor interface — the boundary between
// "the instrument" (everything under src/synth/ and src/dsp/) and "the
// host" (everything under src/host/). It is deliberately shaped like a
// VST3 IComponent / JUCE AudioProcessor so an implementation can later be
// wrapped as a plugin, or moved to another audio backend, without touching
// any DSP code:
//
//   prepare()  <->  VST3 setupProcessing() / JUCE prepareToPlay()
//   process()  <->  VST3 process()         / JUCE processBlock()
//   reset()    <->  VST3 setProcessing(false) / JUCE reset()
//
// Lifecycle (driven by the host):
//
//   construct -> prepare(sr, maxBlock) -> process() x N -> [reset() ->] ...
//
//   - prepare() may be called again with a new sample rate / block size,
//     but never while process() is running.
//   - process() is called on the real-time audio thread, once per block.
//   - reset() drops all sounding state (voices, delay lines, ...) but
//     keeps parameter values; also never concurrent with process().
//
// Threading contract for process() implementations:
//   - no memory allocation, no locks, no blocking I/O, no exceptions —
//     anything that can block stalls the audio driver and causes dropouts
//   - all communication with other threads goes through the MidiBuffer
//     (input events) and the atomic Parameters (control values)
//
// To add a new instrument, subclass Processor (see SineSynthProcessor for
// a fully worked reference implementation) and hand it to StandaloneHost.
class Processor {
public:
    virtual ~Processor() = default;

    // Stable identifier, e.g. "rtsynth.sine" (think plugin ID).
    virtual const char* name() const = 0;

    // Allocate/size all buffers here; after this call, process() must be
    // able to run without allocating. maxBlockSize is an upper bound on
    // the numFrames of any AudioBufferView passed to process().
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;

    // Silence everything, keep parameters.
    virtual void reset() = 0;

    // Render one block into `output`. `midi` holds this block's events in
    // non-decreasing sampleOffset order. Real-time safe (see above).
    virtual void process(AudioBufferView& output, const MidiBuffer& midi) = 0;

    // The processor's public control surface. Hosts/UIs discover and set
    // parameters through this; the audio thread reads them in process().
    ParameterSet& parameters(){ return parameters_; }

    // --- optional hooks a UI (console, LCD, ...) reads --------------------
    //
    // All three are read from a UI thread while audio runs, and none of
    // them is required: an instrument that implements none still works,
    // the display just shows raw numbers and no presets.

    // The instrument's preset bank, or nullptr when it has none. The
    // instrument itself selects slots from MIDI Program Change; a UI uses
    // this to show which preset is live.
    virtual PresetBank* presets(){ return nullptr; }

    // The parameter a MIDI CC currently writes to, or nullptr when the
    // controller is unmapped. Lets a UI report "CC 46 -> L1 DCW Rate 1"
    // without duplicating the instrument's CC map. The answer may depend
    // on instrument state (pd's CC blocks retarget between lines).
    virtual Parameter* parameterForCc(uint8_t /*cc*/){ return nullptr; }

    // Human-readable rendering of a parameter's current value, for
    // parameters whose raw number means nothing on its own ("3: Pulse",
    // "Step 2", "-7 semitones"). An empty string means "no special
    // formatting" and the UI prints the number itself.
    virtual std::string describeValue(const Parameter& /*parameter*/) const { return {}; }

    // Optional diagnostic: number of currently sounding voices, or -1 if
    // the instrument doesn't report it. Read from the main thread while
    // audio runs — an approximate, torn-read-tolerant gauge is fine. A
    // count that climbs and stays pinned at the maximum reveals stuck
    // voices (lost note-offs, stalled envelopes) and the CPU load they
    // cause.
    virtual int activeVoiceCount() const { return -1; }

    // Optional: cap polyphony at runtime. Fewer voices means proportionally
    // less DSP work, which is how an embedded build trades polyphony for
    // headroom when the audio deadline is tight (see RtAudioOutput's load
    // meter). Call before playing; instruments that don't support it ignore
    // the request. Values <= 0 mean "instrument default".
    virtual void setMaxVoices(int /*maxVoices*/){}

protected:
    // Populate in the subclass constructor via parameters_.add(...).
    ParameterSet parameters_;
};

}  // namespace rtsynth
