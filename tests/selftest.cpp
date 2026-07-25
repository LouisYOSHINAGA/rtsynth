// Offline self-test: renders the processor without any audio hardware and
// checks basic invariants (signal appears on note-on, decays after note-off,
// output is finite and clipped, voice stealing stays stable). This is the
// benefit of the Processor abstraction: DSP can be tested headless in CI.

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/core/MidiBuffer.hpp"
#include "../src/core/MidiStreamParser.hpp"
#include "../src/dsp/SmoothedValue.hpp"
#include "../src/host/ControlLoop.hpp"
#include "../src/host/DisplayUi.hpp"
#include "../src/host/GpioEncoderInput.hpp"  // QuadratureDecoder
#include "../src/host/ParameterWatcher.hpp"
#include "../src/synth/SineSynthProcessor.hpp"
#ifdef RTSYNTH_HAVE_PD
#include "../src/synth/PdSynthProcessor.hpp"
#endif

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

float renderBlocks(Processor& synth, int numBlocks, const MidiBuffer& firstBlockMidi){
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

    // hardware-style control input driving a parameter through ControlLoop
    {
        struct FakePot : ControlInput {
            float value = 0.0f;
            const char* name() const override { return "fake"; }
            int numChannels() const override { return 1; }
            bool read(int, float& out) override { out = value; return true; }
        } pot;

        SineSynthProcessor s2;
        Parameter* attack = s2.parameters().byId("attack");
        ControlLoop loop(&pot);
        loop.addMapping(0, attack);

        pot.value = 1.0f;
        for(int i = 0; i < 50; i++){
            loop.pollOnce();
        }
        expect(std::abs(attack->getNormalized() - 1.0f) < 0.01f,
               "control loop drives parameter to knob position");

        const float held = attack->get();
        pot.value = 0.9995f;  // sub-threshold ADC jitter
        for(int i = 0; i < 50; i++){
            loop.pollOnce();
        }
        expect(attack->get() == held, "ADC jitter below threshold is ignored");

        pot.value = 0.0f;
        for(int i = 0; i < 50; i++){
            loop.pollOnce();
        }
        expect(attack->getNormalized() < 0.01f,
               "control loop follows knob back down");
    }

#ifdef RTSYNTH_HAVE_PD
    // the PD synth (external/pd submodule) hosted as a Processor
    {
        PdSynthProcessor pd;
        pd.prepare(kSampleRate, kBlockSize);

        MidiBuffer empty;
        expect(renderBlocks(pd, 4, empty) == 0.0f, "pd: silent before any note");

        StereoBlock block;
        MidiBuffer midi;
        midi.add(MidiEvent::noteOn(0, 60, 100));
        AudioBufferView view = block.view();
        pd.process(view, midi);
        for(int i = 0; i < 10; i++){
            AudioBufferView v = block.view();
            pd.process(v, empty);
        }
        expect(peak(block.left) > 0.01f && peak(block.right) > 0.01f,
               "pd: note-on produces signal on both channels");
        expect(allFinite(block.left) && peak(block.left) <= 1.0f,
               "pd: output is finite and clipped");

        MidiBuffer off;
        off.add(MidiEvent::noteOff(0, 60));
        renderBlocks(pd, 300, off);  // ride out the DCA release
        expect(renderBlocks(pd, 4, empty) == 0.0f, "pd: silent again after release");

        MidiBuffer flood;
        for(uint8_t n = 0; n < 32; n++){
            flood.add(MidiEvent::noteOn(0, static_cast<uint8_t>(40 + n), 100));
        }
        const float p = renderBlocks(pd, 20, flood);
        expect(std::isfinite(p) && p <= 1.0f, "pd: voice stealing is stable and clipped");

        MidiBuffer allOff;
        allOff.add(MidiEvent::controlChange(0, 123, 0));
        renderBlocks(pd, 400, allOff);
        expect(renderBlocks(pd, 4, empty) == 0.0f, "pd: CC123 releases all notes");
    }

    // MIDI CC reaches pd's own EG/line parameters (controller.cpp's
    // getMidiControllerAssignment mapping, reimplemented in handleEvent)
    {
        PdSynthProcessor pd;
        pd.prepare(kSampleRate, kBlockSize);
        MidiBuffer empty;

        // CC14 = DCO EG rate 1 of the edit-target line (line 1 by default)
        Parameter* line1DcoRate1 = pd.parameters().byId("line1_dco_rate1");
        Parameter* line2DcoRate1 = pd.parameters().byId("line2_dco_rate1");
        expect(line1DcoRate1 != nullptr && line2DcoRate1 != nullptr,
               "pd: EG CC target parameters are registered");

        MidiBuffer cc14;
        cc14.add(MidiEvent::controlChange(0, 14, 64));
        renderBlocks(pd, 1, cc14);
        expect(std::abs(line1DcoRate1->getNormalized() - 64.0f / 127.0f) < 0.01f,
               "pd: CC14 sets line1 DCO EG rate 1");

        // CC3 >= 64 switches the CC edit target to line 2
        MidiBuffer cc3;
        cc3.add(MidiEvent::controlChange(0, 3, 127));
        renderBlocks(pd, 1, cc3);
        MidiBuffer cc14b;
        cc14b.add(MidiEvent::controlChange(0, 14, 32));
        renderBlocks(pd, 1, cc14b);
        expect(std::abs(line2DcoRate1->getNormalized() - 32.0f / 127.0f) < 0.01f,
               "pd: CC3 retargets CC14 to line 2");
        expect(std::abs(line1DcoRate1->getNormalized() - 64.0f / 127.0f) < 0.01f,
               "pd: line 1's value is untouched by the retargeted CC14");
    }
#endif

    // rotary encoder path: relative mapping nudges the parameter and clamps
    {
        struct FakeEncoder : RelativeControlInput {
            int pending = 0;
            const char* name() const override { return "fake-enc"; }
            int numChannels() const override { return 1; }
            int readDelta(int) override {
                const int d = pending;
                pending = 0;
                return d;
            }
        } encoder;

        SineSynthProcessor s3;
        Parameter* sustain = s3.parameters().byId("sustain");
        sustain->setNormalized(0.5f);
        ControlLoop loop(nullptr, &encoder);
        loop.addRelativeMapping(0, sustain, 0.01f);

        encoder.pending = +5;
        loop.pollOnce();
        expect(std::abs(sustain->getNormalized() - 0.55f) < 0.001f,
               "encoder detents nudge the parameter");

        encoder.pending = -100;
        loop.pollOnce();
        expect(sustain->getNormalized() == 0.0f,
               "encoder movement clamps at the parameter range");
    }

    // quadrature decoding: one full EC11 cycle in each direction
    {
        QuadratureDecoder decoder;
        decoder.reset(0b00);
        int detents = 0;
        for(uint8_t state : {0b01, 0b11, 0b10, 0b00}){  // clockwise cycle
            detents += decoder.onState(state);
        }
        expect(detents == +1, "quadrature full cycle gives one CW detent");

        detents = 0;
        for(uint8_t state : {0b10, 0b11, 0b01, 0b00}){  // counter-clockwise
            detents += decoder.onState(state);
        }
        expect(detents == -1, "quadrature reverse cycle gives one CCW detent");

        detents = 0;
        for(uint8_t state : {0b01, 0b00, 0b01, 0b00}){  // contact bounce
            detents += decoder.onState(state);
        }
        expect(detents == 0, "contact bounce produces no detent");
    }

    // parameter change watching (the LCD/console hook)
    {
        SineSynthProcessor s4;
        ParameterWatcher watcher(s4.parameters());

        int reported = 0;
        Parameter* changed = nullptr;
        watcher.pollChanges([&](Parameter& p){ reported++; changed = &p; });
        expect(reported == 0, "watcher is silent when nothing changed");

        s4.parameters().byId("decay")->set(0.3f);
        watcher.pollChanges([&](Parameter& p){ reported++; changed = &p; });
        expect(reported == 1 && changed != nullptr && changed->id() == "decay",
               "watcher reports exactly the changed parameter");

        watcher.pollChanges([&](Parameter&){ reported++; });
        expect(reported == 1, "watcher reports each change only once");
    }

    // display UI: last-changed parameter reaches the (fake) LCD
    {
        struct FakeDisplay : TextDisplay {
            std::string lines[2];
            int writes = 0;
            int columns() const override { return 16; }
            int rows() const override { return 2; }
            void writeLine(int row, const std::string& text) override {
                lines[row] = text;
                writes++;
            }
            void clear() override { lines[0].clear(); lines[1].clear(); }
        } fakeLcd;

        SineSynthProcessor s5;
        DisplayUi ui(fakeLcd, s5.parameters());

        ui.pollOnce();
        expect(fakeLcd.writes == 0, "display idles while nothing changes");

        s5.parameters().byId("release")->set(0.25f);
        ui.pollOnce();
        expect(fakeLcd.lines[0] == "release" && fakeLcd.lines[1] == "0.250 s",
               "display shows the last-changed parameter and value");

        const int writesAfterChange = fakeLcd.writes;
        ui.pollOnce();
        expect(fakeLcd.writes == writesAfterChange,
               "display does not redraw without changes");

        Parameter unitless("x", "X", 0.0f, 1.0f, 0.5f);
        expect(formatParameterValue(unitless) == "0.500",
               "value formatting omits the missing unit");
    }

    // raw MIDI byte-stream parsing (the --midi-raw input path)
    {
        MidiStreamParser parser;
        std::vector<MidiEvent> events;
        auto collect = [&](const MidiEvent& e){ events.push_back(e); };

        // dense chord in running status: one 0x90, then note/velocity pairs
        const uint8_t chord[] = {0x90, 60, 100, 62, 101, 64, 102};
        parser.feed(chord, sizeof(chord), collect);
        expect(events.size() == 3
               && events[0].type == MidiEvent::Type::NoteOn && events[0].data1 == 60
               && events[2].data1 == 64 && events[2].data2 == 102,
               "parser: running-status chord yields every note-on");

        // matching releases, also running status, with velocity-0 note-ons
        events.clear();
        const uint8_t offs[] = {0x90, 60, 0, 62, 0, 64, 0};
        parser.feed(offs, sizeof(offs), collect);
        expect(events.size() == 3
               && events[0].type == MidiEvent::Type::NoteOff
               && events[2].type == MidiEvent::Type::NoteOff && events[2].data1 == 64,
               "parser: velocity-0 running status yields every note-off");

        // a real-time byte (0xF8 clock) inside a message must be transparent
        events.clear();
        const uint8_t interleaved[] = {0x80, 60, 0xF8, 64};
        parser.feed(interleaved, sizeof(interleaved), collect);
        expect(events.size() == 1
               && events[0].type == MidiEvent::Type::NoteOff && events[0].data1 == 60,
               "parser: real-time byte inside a message is transparent");

        // sysex is skipped and does not desynchronize the stream
        events.clear();
        const uint8_t sysex[] = {0xF0, 0x7E, 0x7F, 0x09, 0xF7, 0x90, 61, 99};
        parser.feed(sysex, sizeof(sysex), collect);
        expect(events.size() == 1 && events[0].data1 == 61,
               "parser: sysex is skipped without losing framing");

        // unsupported one-data-byte message (program change) is framed away
        events.clear();
        const uint8_t progChange[] = {0xC0, 5, 0x90, 62, 98};
        parser.feed(progChange, sizeof(progChange), collect);
        expect(events.size() == 1 && events[0].data1 == 62,
               "parser: program change is framed and skipped");

        // byte-by-byte delivery (raw reads can split anywhere)
        events.clear();
        const uint8_t split[] = {0xB0, 7, 100};
        for(uint8_t byte : split){
            parser.feedByte(byte, collect);
        }
        expect(events.size() == 1
               && events[0].type == MidiEvent::Type::ControlChange
               && events[0].data1 == 7 && events[0].data2 == 100,
               "parser: messages split across reads reassemble");
    }

    // per-sample gain smoothing converges without overshoot
    {
        SmoothedValue smoothed;
        smoothed.prepare(kSampleRate, 0.005f);
        smoothed.snap(0.0f);
        smoothed.setTarget(1.0f);
        bool monotone = true;
        float previous = 0.0f;
        for(int i = 0; i < 4410; i++){  // 100 ms
            const float value = smoothed.tick();
            if(value < previous || value > 1.0f){
                monotone = false;
            }
            previous = value;
        }
        expect(monotone && previous > 0.99f,
               "smoothed value ramps monotonically to its target");
    }

    std::printf("%s (%d failure%s)\n",
                (g_failures == 0)? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures, (g_failures == 1)? "" : "s");
    return (g_failures == 0)? 0 : 1;
}
