// Offline self-test: renders the processor without any audio hardware and
// checks basic invariants (signal appears on note-on, decays after note-off,
// output is finite and clipped, voice stealing stays stable). This is the
// benefit of the Processor abstraction: DSP can be tested headless in CI.

#include <cmath>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/core/MidiBuffer.hpp"
#include "../src/core/MidiStreamParser.hpp"
#include "../src/core/PresetBank.hpp"
#include "../src/core/SpscRingBuffer.hpp"
#include "../src/dsp/SmoothedValue.hpp"
#include "../src/host/ControlLoop.hpp"
#include "../src/host/GpioEncoderInput.hpp"  // QuadratureDecoder
#include "../src/host/LcdParameterDisplay.hpp"
#include "../src/host/ParameterDisplay.hpp"
#include "../src/host/ParameterMonitor.hpp"
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

    // CC assignments added by the pd submodule bump: line select, detune,
    // waveform and the Mono/Poly Mode On messages
    {
        PdSynthProcessor pd;
        pd.prepare(kSampleRate, kBlockSize);

        MidiBuffer cc;
        cc.add(MidiEvent::controlChange(0, 9, 127));    // line select -> 1+2'
        cc.add(MidiEvent::controlChange(0, 87, 100));   // detune fine
        cc.add(MidiEvent::controlChange(0, 89, 40));    // waveform 1st
        cc.add(MidiEvent::controlChange(0, 126, 0));    // Mono Mode On
        renderBlocks(pd, 1, cc);

        expect(pd.parameters().byId("line_select")->getNormalized() == 127.0f / 127.0f,
               "pd: CC9 selects the played line");
        expect(std::abs(pd.parameters().byId("detune_fine")->getNormalized()
                        - 100.0f / 127.0f) < 0.01f,
               "pd: CC87 sets detune fine");
        expect(std::abs(pd.parameters().byId("line1_wave1")->getNormalized()
                        - 40.0f / 127.0f) < 0.01f,
               "pd: CC89 sets the edit line's first waveform");
        // the data byte of Mono Mode On is a channel count, not a value
        expect(pd.parameters().byId("mono")->get() == 1.0f,
               "pd: CC126 switches to mono whatever its data byte says");

        MidiBuffer poly;
        poly.add(MidiEvent::controlChange(0, 127, 0));  // Poly Mode On
        renderBlocks(pd, 1, poly);
        expect(pd.parameters().byId("mono")->get() == 0.0f,
               "pd: CC127 switches back to poly");
    }

    // presets: Program Change switches slots and keeps each slot's edits
    {
        PdSynthProcessor pd;
        pd.prepare(kSampleRate, kBlockSize);

        PresetBank* bank = pd.presets();
        expect(bank != nullptr && bank->count() > 1 && bank->current() == 0,
               "pd: a factory bank with several presets starts on slot 0");

        Parameter* rate = pd.parameters().byId("line1_dca_rate1");
        const float slot0 = rate->get();

        MidiBuffer edit;
        edit.add(MidiEvent::controlChange(0, 102, 20));  // DCA EG rate 1
        renderBlocks(pd, 1, edit);
        const float edited = rate->get();
        expect(edited != slot0, "pd: CC edits the live preset");

        MidiBuffer program;
        program.add(MidiEvent::programChange(0, 1));
        renderBlocks(pd, 1, program);
        expect(bank->current() == 1 && rate->get() != edited,
               "pd: program change loads another preset");

        MidiBuffer back;
        back.add(MidiEvent::programChange(0, 0));
        renderBlocks(pd, 1, back);
        expect(bank->current() == 0 && rate->get() == edited,
               "pd: returning to a preset restores the edits made to it");

        MidiBuffer unknown;
        unknown.add(MidiEvent::programChange(0, 120));
        renderBlocks(pd, 1, unknown);
        expect(bank->current() == 0, "pd: an out-of-range program change is ignored");

    }

    // panel preset buttons: the ring order they step through. Buttons send
    // the slot neighbour() names as a Program Change, so this is the whole
    // of "next"/"previous" including the wrap at both ends.
    {
        ParameterSet parameters;
        parameters.add("a", "A", 0.0f, 1.0f, 0.25f);
        parameters.add("b", "B", 0.0f, 1.0f, 0.5f);
        PresetBank bank(parameters);
        expect(bank.neighbour(+1) == 0 && bank.neighbour(-1) == 0,
               "presets: an empty bank has nowhere to step");

        bank.add("one");
        bank.add("two");
        bank.add("three");

        expect(bank.current() == 0, "presets: a new bank starts on slot 0");
        expect(bank.neighbour(+1) == 1, "presets: next steps forward");
        expect(bank.neighbour(-1) == 2, "presets: previous from the first wraps to the last");

        bank.select(bank.neighbour(-1));
        expect(bank.current() == 2, "presets: stepping back from slot 0 selects the last");
        expect(bank.neighbour(+1) == 0, "presets: next from the last wraps to the first");

        bank.select(bank.neighbour(+1));
        expect(bank.current() == 0, "presets: stepping forward from the last selects the first");

        // pressing next N times must visit every slot and come home
        for(int i = 0; i < bank.count(); i++){
            bank.select(bank.neighbour(+1));
        }
        expect(bank.current() == 0, "presets: a full lap of next returns to the start");
    }

    // every factory preset must actually make a sound (fresh instrument, so
    // no edits from the case above are carried in)
    {
        PdSynthProcessor pd;
        pd.prepare(kSampleRate, kBlockSize);
        PresetBank* bank = pd.presets();
        for(int i = 0; i < bank->count(); i++){
            bank->select(i);
            MidiBuffer note;
            note.add(MidiEvent::noteOn(0, 60, 100));
            const float p = renderBlocks(pd, 40, note);
            expect(p > 0.01f && std::isfinite(p) && p <= 1.0f,
                   ("pd: preset sounds: " + bank->name(i)).c_str());
            MidiBuffer off;
            off.add(MidiEvent::controlChange(0, 123, 0));
            renderBlocks(pd, 300, off);
        }
    }

    // what a display shows: the CC's target parameter and its decoded value
    {
        PdSynthProcessor pd;
        Parameter* target = pd.parameterForCc(46);  // DCW EG rate 1, line 1
        expect(target != nullptr && target->id() == "line1_dcw_rate1",
               "pd: a CC reports the parameter it writes to");
        expect(pd.parameterForCc(1) == nullptr,   // mod wheel: pd maps nothing to it
               "pd: an unmapped CC reports no parameter");

        pd.parameters().byId("line1_wave1")->setNormalized(3.0f / 7.0f);
        expect(pd.describeValue(*pd.parameters().byId("line1_wave1")) == "Double Sine",
               "pd: discrete values are described by name");
        expect(pd.describeValue(*pd.parameters().byId("mono")) == "POLY",
               "pd: switches are described by position");
    }

    // ParameterMonitor: one line carrying both the CC and the value it made
    {
        struct RecordingDisplay : ParameterDisplay {
            std::vector<DisplayLine> lines;
            std::vector<std::string> presets;
            void showParameter(const DisplayLine& line) override { lines.push_back(line); }
            void showPreset(int index, const std::string& name) override {
                presets.push_back(std::to_string(index) + ":" + name);
            }
        } display;

        PdSynthProcessor pd;
        pd.prepare(kSampleRate, kBlockSize);
        ParameterMonitor monitor(pd);
        monitor.addDisplay(&display);

        const MidiEvent cc = MidiEvent::controlChange(0, 46, 100);
        MidiBuffer midi;
        midi.add(cc);
        renderBlocks(pd, 1, midi);
        monitor.noteMidiEvent(cc);
        monitor.poll();
        expect(display.lines.size() == 1
               && display.lines[0].id == "line1_dcw_rate1"
               && display.lines[0].source == "CC 46 = 100"
               && !display.lines[0].value.empty(),
               "monitor: a CC and the parameter value it produced share one line");

        display.lines.clear();
        monitor.poll();
        expect(display.lines.empty(), "monitor: nothing is reported twice");

        // a preset replaces every value at once: report the preset, not ~120 lines
        MidiBuffer program;
        program.add(MidiEvent::programChange(0, 2));
        renderBlocks(pd, 1, program);
        monitor.poll();
        expect(display.lines.empty() && display.presets.size() == 1
               && display.presets[0] == "2:E.Piano",
               "monitor: a preset switch is reported as one preset line");
    }
#endif

    // Everything the hardware synth can be asked to show must fit the 16
    // columns of its LCD. LcdParameterDisplay truncates silently, so an
    // overlong name or value does not fail anywhere — it just reaches the
    // instrument as a cut-off word ("Resonance III Tra"). Sweep every
    // the user slots: last in the bank so the factory Program Change
    // numbers stay put, and holding nothing but the plain defaults
    {
        PdSynthProcessor pd;
        PresetBank* bank = pd.presets();
        expect(bank->count() >= 4, "pd: the bank has factory slots plus user slots");
        const int firstUser = bank->count() - 3;
        expect(bank->name(firstUser) == "User 1"
               && bank->name(firstUser + 1) == "User 2"
               && bank->name(firstUser + 2) == "User 3",
               "pd: three user slots sit at the end of the bank");

        // slot 0 ("Init Saw") is the bare default patch, so a user slot
        // must load exactly the same values as it
        bank->select(0);
        std::vector<float> defaults;
        for(auto& parameter : pd.parameters()){
            defaults.push_back(parameter->get());
        }
        bank->select(firstUser);
        bool same = true;
        size_t index = 0;
        for(auto& parameter : pd.parameters()){
            same = same && parameter->get() == defaults[index++];
        }
        expect(same, "pd: a user slot starts from the plain defaults");
    }

    // parameter across its whole range, plus every preset, and measure.
    {
        struct WidestDisplay : ParameterDisplay {
            size_t widest = 0;
            std::string worst;
            void note(const std::string& text){
                if(text.size() > widest){
                    widest = text.size();
                    worst = text;
                }
            }
            void showParameter(const DisplayLine& line) override {
                note(line.label.empty()? line.id : line.label);
                note(line.value);
            }
            void showPreset(int index, const std::string& name) override {
                note("PRESET " + std::to_string(index));
                note(name);
            }
        };

        // 240 steps resolves every discrete option any parameter has (the
        // finest is detune fine's 121 positions), so each name the synth
        // can print for a value is actually visited.
        auto sweep = [](Processor& processor, WidestDisplay& widest){
            ParameterMonitor monitor(processor);
            monitor.addDisplay(&widest);
            for(auto& parameter : processor.parameters()){
                for(int step = 0; step <= 240; step++){
                    parameter->setNormalized(static_cast<float>(step) / 240.0f);
                    monitor.poll();
                }
            }
            if(PresetBank* bank = processor.presets()){
                for(int slot = 0; slot < bank->count(); slot++){
                    bank->select(slot);
                    monitor.poll();
                }
            }
        };

        WidestDisplay sine;
        SineSynthProcessor s6;
        sweep(s6, sine);
        expect(sine.widest <= 16,
               "sine: every displayed line fits a 16-column LCD");
        if(sine.widest > 16){
            std::printf("       widest was %zu: \"%s\"\n", sine.widest, sine.worst.c_str());
        }

#ifdef RTSYNTH_HAVE_PD
        WidestDisplay pd;
        PdSynthProcessor p2;
        p2.prepare(kSampleRate, kBlockSize);
        sweep(p2, pd);
        expect(pd.widest <= 16,
               "pd: every displayed line fits a 16-column LCD");
        if(pd.widest > 16){
            std::printf("       widest was %zu: \"%s\"\n", pd.widest, pd.worst.c_str());
        }
#endif
    }

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

    // LCD backend: the monitor's lines reach the (fake) character display
    {
        struct FakeDisplay : TextDisplay {
            std::string lines[2];
            int columns() const override { return 16; }
            int rows() const override { return 2; }
            void writeLine(int row, const std::string& text) override {
                lines[row] = text;
            }
            void clear() override { lines[0].clear(); lines[1].clear(); }
        } fakeLcd;

        SineSynthProcessor s5;
        LcdParameterDisplay lcd(fakeLcd);
        ParameterMonitor monitor(s5);
        monitor.addDisplay(&lcd);

        monitor.poll();
        expect(lcd.writeCount() == 0, "lcd idles while nothing changes");

        s5.parameters().byId("release")->set(0.25f);
        monitor.poll();
        expect(fakeLcd.lines[0] == "Release" && fakeLcd.lines[1] == "0.25 s",
               "lcd shows the last-changed parameter and its value");

        // a knob sweep moves the value under an unchanged name: only the
        // value row may be redrawn, the I2C write for the name is saved
        const int writesAfterFirstDraw = lcd.writeCount();
        s5.parameters().byId("release")->set(0.5f);
        monitor.poll();
        expect(fakeLcd.lines[1] == "0.5 s"
               && lcd.writeCount() == writesAfterFirstDraw + 1,
               "lcd redraws only the row that changed");

        monitor.poll();
        expect(lcd.writeCount() == writesAfterFirstDraw + 1,
               "lcd does not redraw without changes");

        DisplayLine wide;
        wide.id = "long_parameter_identifier";
        wide.label = "A Name Longer Than Sixteen";
        wide.value = "1234567890123456789";
        lcd.showParameter(wide);
        expect(fakeLcd.lines[0] == "A Name Longer Th"
               && fakeLcd.lines[1] == "1234567890123456",
               "lcd truncates both rows to the display width");

        lcd.showPreset(2, "E.Piano");
        expect(fakeLcd.lines[0] == "PRESET 2" && fakeLcd.lines[1] == "E.Piano",
               "lcd shows a preset switch on both rows");
    }

    // regression: when voice stealing leaves TWO voices on the same note,
    // one note-off must silence both. Reproduced deterministically:
    // fill the pool, release two voices, steal the oldest for note 63
    // (which fades before sounding), let the other one go free during that
    // fade, then press 63 again so it lands on the freed voice — the
    // stolen voice starts the same note when its fade ends.
    {
        VoiceAllocator<3> voices;
        voices.prepare(kSampleRate);
        voices.setEnvelopeParameters(0.001f, 0.001f, 1.0f, 0.001f);

        float scratch[64];
        auto render = [&](int frames){
            for(int i = 0; i < frames; i++){
                scratch[i] = 0.0f;
            }
            voices.render(scratch, frames, 1.0);
        };

        voices.noteOn(60, 100);   // oldest
        voices.noteOn(61, 100);
        voices.noteOn(62, 100);   // newest; stays held throughout
        render(64);
        expect(voices.activeCount() == 3, "pool is full before stealing");

        voices.noteOff(60);       // both start releasing (~44 frames)
        voices.noteOff(61);
        voices.noteOn(63, 100);   // no free voice -> steals the oldest (60)
        render(50);               // 61's voice frees; 60's steal fade (~132) runs
        voices.noteOn(63, 100);   // same note again -> lands on the freed voice
        for(int i = 0; i < 4; i++){
            render(64);           // the stolen voice finishes fading and starts 63
        }
        expect(voices.activeCount() == 3, "two voices ended up holding note 63");

        voices.noteOff(63);
        for(int i = 0; i < 8; i++){
            render(64);
        }
        expect(voices.activeCount() == 1,
               "one note-off silences every voice holding that note");
    }

    // runtime polyphony cap (--voices): never exceed it, still fully usable
    {
        SineSynthProcessor capped;
        capped.setMaxVoices(4);
        capped.prepare(kSampleRate, kBlockSize);

        MidiBuffer midi;
        for(uint8_t n = 0; n < 12; n++){
            midi.add(MidiEvent::noteOn(0, static_cast<uint8_t>(50 + n), 100));
        }
        const float p = renderBlocks(capped, 10, midi);
        expect(std::isfinite(p) && p > 0.01f, "capped synth still sounds");
        expect(capped.activeVoiceCount() <= 4, "polyphony cap is respected");

        MidiBuffer allOff;
        allOff.add(MidiEvent::controlChange(0, 123, 0));
        renderBlocks(capped, 300, allOff);
        MidiBuffer empty;
        expect(renderBlocks(capped, 4, empty) == 0.0f,
               "capped synth releases all notes");
    }

    // The MIDI thread -> audio thread handoff must never lose, duplicate or
    // reorder an event: a single lost note-off leaves a note droning, and
    // this queue is the only lock-free component on that path. Hammered
    // here with a real producer thread and a deliberately small buffer so
    // wrap-around and full conditions occur constantly.
    {
        constexpr int kEvents = 200000;
        SpscRingBuffer<MidiEvent, 64> queue;
        std::atomic<bool> producerDone{false};

        std::thread producer([&]{
            for(int i = 0; i < kEvents; i++){
                MidiEvent event = MidiEvent::noteOn(0, 60, 100);
                event.sampleOffset = i;              // sequence number
                while(!queue.push(event)){           // spin while full
                    std::this_thread::yield();
                }
            }
            producerDone.store(true);
        });

        int received = 0;
        bool ordered = true;
        while(received < kEvents){
            MidiEvent event;
            if(queue.pop(event)){
                if(event.sampleOffset != received){
                    ordered = false;                 // lost, duplicated or reordered
                    break;
                }
                received++;
            }else if(producerDone.load() && received < kEvents){
                std::this_thread::yield();           // drain the tail
            }
        }
        producer.join();

        expect(ordered && received == kEvents,
               "SPSC queue delivers every event exactly once, in order");
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

        // one-data-byte message (program change): decoded, and its shorter
        // framing must not eat the note that follows
        events.clear();
        const uint8_t progChange[] = {0xC0, 5, 0x90, 62, 98};
        parser.feed(progChange, sizeof(progChange), collect);
        expect(events.size() == 2
               && events[0].type == MidiEvent::Type::ProgramChange && events[0].data1 == 5
               && events[1].type == MidiEvent::Type::NoteOn && events[1].data1 == 62,
               "parser: program change is decoded without losing framing");

        // a truncated system-common message must not eat the next message's
        // data byte (F2 promises two data bytes but is cut short here)
        events.clear();
        const uint8_t truncatedCommon[] = {0xF2, 0x10, 0x90, 63, 97};
        parser.feed(truncatedCommon, sizeof(truncatedCommon), collect);
        expect(events.size() == 1
               && events[0].type == MidiEvent::Type::NoteOn
               && events[0].data1 == 63 && events[0].data2 == 97,
               "parser: truncated system common does not corrupt the next note");

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
