#pragma once

#include <vector>
#include "../core/Processor.hpp"
#include "../dsp/SmoothedValue.hpp"
#include "VoiceAllocator.hpp"

namespace rtsynth {

// The actual instrument — and the reference for writing your own
// Processor. A 16-voice sine synth with ADSR amplitude envelopes,
// velocity sensitivity, pitch bend and sustain pedal.
//
// Signal flow per block (see process() in the .cpp):
//
//   MidiBuffer --> VoiceAllocator (note on/off, CC, bend)
//                       |
//   16 x Voice --(+=)--> mono scratch buffer --gain/clip--> all channels
//
// Everything platform-specific lives in the host layer; this class only
// sees AudioBufferView + MidiBuffer, so it can be wrapped as a VST3/JUCE
// plugin (or run on another backend) unchanged.
//
// To build a different instrument, copy this class and swap the voice
// internals (Voice/dsp components); the MIDI dispatch, sample-accurate
// segmentation and parameter plumbing can usually stay as they are.
class SineSynthProcessor : public Processor {
public:
    static constexpr size_t kNumVoices = 16;

    SineSynthProcessor();

    const char* name() const override { return "rtsynth.sine"; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBufferView& output, const MidiBuffer& midi) override;

private:
    void handleEvent(const MidiEvent& event);
    void renderSegment(int startFrame, int numFrames);

    VoiceAllocator<kNumVoices> voices_;
    std::vector<float> monoScratch_;   // sized in prepare(); reused every block
    SmoothedValue gainSmoother_;       // per-sample gain ramp (anti-zipper)
    double sampleRate_ = 44100.0;
    double pitchBendRatio_ = 1.0;

    // parameter handles (owned by ParameterSet, valid for our lifetime)
    Parameter* gain_ = nullptr;
    Parameter* attack_ = nullptr;
    Parameter* decay_ = nullptr;
    Parameter* sustain_ = nullptr;
    Parameter* release_ = nullptr;
    Parameter* bendRange_ = nullptr;
};

}  // namespace rtsynth
