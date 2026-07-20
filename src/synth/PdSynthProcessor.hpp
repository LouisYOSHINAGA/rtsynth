#pragma once

#include <array>
#include <string>
#include <vector>

#include "../core/Processor.hpp"
#include "../dsp/SmoothedValue.hpp"

// DSP core of the LouisYOSHINAGA/pd VST3 plugin (git submodule external/pd).
// These headers depend on the VST3 SDK only through the type aliases in
// pluginterfaces/vst/vsttypes.h, which src/vst3shim/ provides here.
#include "const.h"
#include "voice.h"

namespace rtsynth {

// Hosts the CZ-style phase distortion synth from the pd repository as an
// rtsynth Processor — the standalone counterpart of pd's PDProcessor
// (processor.cpp), minus the VST3 plumbing (state streams, oscilloscope
// messages, host parameter queues).
//
// The pd DSP core (PD lines, 8-step EGs, Voice) is used completely
// unmodified via the external/pd submodule; what this class reimplements
// is only the hosting logic: parameter dispatch, the voice pool with
// oldest-voice stealing, mono (SOLO) last-note priority, detune, pitch
// bend and the output mix. All of pd's parameters are registered in the
// ParameterSet as normalized [0,1] values (same encoding as the plugin),
// so MIDI CC, --param and --adc can address every one of them, e.g.
// "line1_dcw_rate1" or "detune_fine".
//
// Unlike the plugin (whose fresh state is silent until the editor pushes
// values), the parameter defaults here form a small init patch — sawtooth
// on line 1 with a fast-attack DCA and a DCW sweep — so the synth makes a
// sound out of the box.
//
// Known caveat inherited from pd: switching a waveform parameter allocates
// the new generator object on the audio thread (PD::setWaveformFirst uses
// make_unique). Fine for patch editing; avoid automating waveform switches
// per-block on a real-time box.
class PdSynthProcessor : public Processor {
public:
    PdSynthProcessor();

    const char* name() const override { return "rtsynth.pd"; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBufferView& output, const MidiBuffer& midi) override;

private:
    using PdVoice = Steinberg::Vst::Voice;
    using LineSelect = Steinberg::Vst::LineSelect;
    static constexpr int kNumPdParams = Steinberg::Vst::kNumParams;
    static constexpr int kMaxVoices = Steinberg::Vst::kMaxVoices;

    struct HeldNote {
        int channel;
        int note;
    };

    // parameter registration (constructor only)
    void registerParameters();
    static std::string lineParamId(int line, int offset);
    static double defaultParamValue(int paramId);

    // mirror of PDProcessor::applyParameter: dispatch one normalized value
    void applyParameter(int paramId, double value);
    // push every Parameter whose value changed since the last block into
    // the voices (the standalone equivalent of the host's parameter queue)
    void syncParameters();
    // set a parameter from MIDI and apply it immediately (mid-block)
    void setAndApply(int paramId, double value);

    void handleEvent(const MidiEvent& event);
    void renderSegment(int startFrame, int numFrames);

    void onNoteOn(int channel, int note);
    void onNoteOff(int channel, int note);
    void releaseAllVoices();
    int effectiveMaxVoices() const;
    PdVoice* allocateVoice();
    void updateDetune();

    std::array<PdVoice, kMaxVoices> voices_;
    std::array<Parameter*, kNumPdParams> paramHandles_{};
    std::array<double, kNumPdParams> appliedValues_{};
    std::vector<HeldNote> heldNotes_;   // mono (SOLO) mode, capacity reserved
    std::vector<float> monoScratch_;
    SmoothedValue volumeSmoother_;

    double pitchBend_ = 0.0;            // [-1, 1], scaled inside Voice
    double volume_ = 0.5;
    LineSelect lineSelect_ = LineSelect::kLine1;
    bool mono_ = false;
    int detuneOctave_ = 0;
    int detuneNote_ = 0;
    int detuneFine_ = 0;
    uint64_t nextVoiceAge_ = 0;
};

}  // namespace rtsynth
