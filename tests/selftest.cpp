// Offline self-test: renders the processor without any audio hardware and
// checks basic invariants (signal appears on note-on, decays after note-off,
// output is finite and clipped, voice stealing stays stable). This is the
// benefit of the Processor abstraction: DSP can be tested headless in CI.

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/core/MidiBuffer.hpp"
#include "../src/synth/SineSynthProcessor.hpp"

using namespace rtsynth;

namespace {

int g_failures = 0;

void expect(bool condition, const char* label){
    std::printf("%s %s\n", condition? "[ok]  " : "[FAIL]", label);
    if(!condition){
        g_failures++;
    }
}

constexpr int kBlockSize = 256;
constexpr double kSampleRate = 44100.0;

struct StereoBlock {
    std::vector<float> left = std::vector<float>(kBlockSize, 0.0f);
    std::vector<float> right = std::vector<float>(kBlockSize, 0.0f);
    float* pointers[2] = {left.data(), right.data()};

    AudioBufferView view(){ return AudioBufferView(pointers, 2, kBlockSize); }
};

float peak(const std::vector<float>& buffer){
    float p = 0.0f;
    for(float s : buffer){
        p = std::max(p, std::abs(s));
    }
    return p;
}

bool allFinite(const std::vector<float>& buffer){
    for(float s : buffer){
        if(!std::isfinite(s)){
            return false;
        }
    }
    return true;
}

float renderBlocks(SineSynthProcessor& synth, int numBlocks, const MidiBuffer& firstBlockMidi){
    StereoBlock block;
    MidiBuffer empty;
    float maxPeak = 0.0f;
    for(int i = 0; i < numBlocks; i++){
        AudioBufferView view = block.view();
        synth.process(view, (i == 0)? firstBlockMidi : empty);
        if(!allFinite(block.left) || !allFinite(block.right)){
            return NAN;
        }
        maxPeak = std::max(maxPeak, peak(block.left));
    }
    return maxPeak;
}

}  // namespace

int main(){
    SineSynthProcessor synth;
    synth.prepare(kSampleRate, kBlockSize);

    // silence before any event
    {
        MidiBuffer midi;
        const float p = renderBlocks(synth, 4, midi);
        expect(p == 0.0f, "silent before any note");
    }

    // note-on produces signal on both channels
    {
        MidiBuffer midi;
        midi.add(MidiEvent::noteOn(0, 69, 100));
        StereoBlock block;
        MidiBuffer empty;
        AudioBufferView view = block.view();
        synth.process(view, midi);
        for(int i = 0; i < 10; i++){
            AudioBufferView v = block.view();
            synth.process(v, empty);
        }
        expect(peak(block.left) > 0.01f, "note-on produces signal");
        expect(peak(block.right) > 0.01f, "signal on both channels");
        expect(allFinite(block.left), "output is finite");
        expect(peak(block.left) <= 1.0f, "output is clipped to [-1, 1]");
    }

    // note-off releases to silence
    {
        MidiBuffer midi;
        midi.add(MidiEvent::noteOff(0, 69));
        const float releasePeak = renderBlocks(synth, 200, midi);  // > 1 s
        expect(std::isfinite(releasePeak), "release output is finite");

        MidiBuffer empty;
        const float tailPeak = renderBlocks(synth, 4, empty);
        expect(tailPeak == 0.0f, "silent again after release");
    }

    // hammer the allocator: many overlapping notes must stay stable
    {
        synth.reset();
        MidiBuffer midi;
        for(uint8_t n = 0; n < 64; n++){
            midi.add(MidiEvent::noteOn(0, static_cast<uint8_t>(30 + n), 100));
        }
        const float p = renderBlocks(synth, 20, midi);
        expect(std::isfinite(p) && p <= 1.0f, "voice stealing is stable and clipped");

        MidiBuffer allOff;
        allOff.add(MidiEvent::controlChange(0, 123, 0));  // all notes off
        renderBlocks(synth, 300, allOff);
        MidiBuffer empty;
        expect(renderBlocks(synth, 4, empty) == 0.0f, "CC123 releases all notes");
    }

    // sustain pedal holds notes across note-off
    {
        synth.reset();
        MidiBuffer midi;
        midi.add(MidiEvent::controlChange(0, 64, 127));  // pedal down
        midi.add(MidiEvent::noteOn(0, 60, 100));
        midi.add(MidiEvent::noteOff(0, 60, kBlockSize - 1));
        renderBlocks(synth, 10, midi);
        MidiBuffer empty;
        const float held = renderBlocks(synth, 10, empty);
        expect(held > 0.01f, "sustain pedal holds released note");

        MidiBuffer pedalUp;
        pedalUp.add(MidiEvent::controlChange(0, 64, 0));
        renderBlocks(synth, 300, pedalUp);
        expect(renderBlocks(synth, 4, empty) == 0.0f, "pedal release ends note");
    }

    // a note stolen while fading must not get stuck when released early
    {
        synth.reset();
        MidiBuffer midi;
        for(uint8_t n = 0; n < 16; n++){
            midi.add(MidiEvent::noteOn(0, static_cast<uint8_t>(40 + n), 100));
        }
        midi.add(MidiEvent::noteOn(0, 100, 100));  // steals a voice (pending)
        midi.add(MidiEvent::noteOff(0, 100, kBlockSize - 1));  // released during fade
        renderBlocks(synth, 5, midi);

        MidiBuffer offs;
        for(uint8_t n = 0; n < 16; n++){
            offs.add(MidiEvent::noteOff(0, static_cast<uint8_t>(40 + n)));
        }
        renderBlocks(synth, 300, offs);
        MidiBuffer empty;
        expect(renderBlocks(synth, 4, empty) == 0.0f, "no stuck note after steal + early note-off");
    }

    // a stolen note must actually start sounding after the fade
    {
        synth.reset();
        MidiBuffer midi;
        for(uint8_t n = 0; n < 16; n++){
            midi.add(MidiEvent::noteOn(0, static_cast<uint8_t>(40 + n), 100));
        }
        midi.add(MidiEvent::noteOn(0, 100, 100));
        renderBlocks(synth, 5, midi);  // fade (3 ms) completes in here

        MidiBuffer offs;
        for(uint8_t n = 0; n < 16; n++){
            offs.add(MidiEvent::noteOff(0, static_cast<uint8_t>(40 + n)));
        }
        renderBlocks(synth, 300, offs);  // original notes fully released
        MidiBuffer empty;
        expect(renderBlocks(synth, 4, empty) > 0.01f, "stolen note sounds after fade");
        MidiBuffer lastOff;
        lastOff.add(MidiEvent::noteOff(0, 100));
        renderBlocks(synth, 300, lastOff);
        expect(renderBlocks(synth, 4, empty) == 0.0f, "stolen note releases normally");
    }

    // pitch bend changes frequency without blowing up
    {
        synth.reset();
        MidiBuffer midi;
        midi.add(MidiEvent::noteOn(0, 69, 100));
        midi.add(MidiEvent::pitchBend(0, 16383, 1));
        const float p = renderBlocks(synth, 10, midi);
        expect(std::isfinite(p) && p > 0.01f, "pitch bend renders normally");
    }

    std::printf("%s (%d failure%s)\n",
                (g_failures == 0)? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures, (g_failures == 1)? "" : "s");
    return (g_failures == 0)? 0 : 1;
}
