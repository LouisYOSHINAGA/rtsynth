// Offline self-test: renders the processor without any audio hardware and
// checks basic invariants (signal appears on note-on, decays after note-off,
// output is finite and clipped, voice stealing stays stable). This is the
// benefit of the Processor abstraction: DSP can be tested headless in CI.

#include <cmath>
#include <atomic>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/MidiBuffer.hpp"
#include "../src/core/MidiStreamParser.hpp"
#include "../src/core/PresetBank.hpp"
#include "../src/core/SpscRingBuffer.hpp"
#include "../src/dsp/SmoothedValue.hpp"
#include "../src/host/ControlLoop.hpp"
#include "../src/host/Mcp23017Panel.hpp"
#include "../src/host/PanelConfig.hpp"
#include "../src/host/PanelLogic.hpp"
#include "../src/host/PdPanel.hpp"
#include "../src/host/QuadratureDecoder.hpp"
#include "../src/host/ShiftRegisterLeds.hpp"
#include "../src/host/LcdParameterDisplay.hpp"
#include "../src/host/ParameterDisplay.hpp"
#include "../src/host/ParameterMonitor.hpp"
#include "../src/host/ParameterWatcher.hpp"
#include "../src/synth/SineSynthProcessor.hpp"
#ifdef RTSYNTH_HAVE_PD
#include "../src/synth/PdPresets.hpp"
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

        // These check the plumbing, not a sound, so pin them to the init
        // patch: a factory preset with a one-second brass attack has barely
        // started by the time they look, and the bank is regenerated from
        // the plugin's own preset files.
        PresetBank* bank = pd.presets();
        for(int slot = 0; slot < bank->count(); slot++){
            if(bank->name(slot) == "Init"){
                bank->select(slot);
                break;
            }
        }

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

        // a switch is staged behind a short fade, so give it a few blocks
        MidiBuffer program;
        program.add(MidiEvent::programChange(0, 1));
        renderBlocks(pd, 4, program);
        expect(bank->current() == 1 && rate->get() != edited,
               "pd: program change loads another preset");

        MidiBuffer back;
        back.add(MidiEvent::programChange(0, 0));
        renderBlocks(pd, 4, back);
        expect(bank->current() == 0 && rate->get() == edited,
               "pd: returning to a preset restores the edits made to it");

        MidiBuffer unknown;
        unknown.add(MidiEvent::programChange(0, 120));
        renderBlocks(pd, 4, unknown);
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

    // Switching preset under sounding voices: the new waveform and
    // envelopes would otherwise be applied to them, turning a release tail
    // into the new sound re-attacking.
    {
        PdSynthProcessor pd;
        pd.prepare(kSampleRate, kBlockSize);
        PresetBank* bank = pd.presets();

        MidiBuffer chord;
        chord.add(MidiEvent::noteOn(0, 60, 110));
        chord.add(MidiEvent::noteOn(0, 64, 110));
        chord.add(MidiEvent::noteOn(0, 67, 110));
        expect(renderBlocks(pd, 60, chord) > 0.001f && pd.activeVoiceCount() == 3,
               "switch: a held chord is sounding");

        // keys still down, so nothing would stop on its own
        MidiBuffer program;
        program.add(MidiEvent::programChange(0, 1));
        MidiBuffer empty;
        renderBlocks(pd, 4, program);
        expect(bank->current() == 1, "switch: the program change still lands");
        expect(pd.activeVoiceCount() == 0,
               "switch: the voices are dropped, not carried into the new sound");
        expect(renderBlocks(pd, 20, empty) == 0.0f,
               "switch: nothing re-attacks afterwards");

        // and the instrument still plays on the new preset
        MidiBuffer again;
        again.add(MidiEvent::noteOn(0, 60, 110));
        expect(renderBlocks(pd, 60, again) > 0.001f,
               "switch: the new preset plays");

        // a program change to the slot already selected must not cut notes
        MidiBuffer same;
        same.add(MidiEvent::programChange(0, 1));
        renderBlocks(pd, 4, same);
        expect(pd.activeVoiceCount() == 1,
               "switch: reselecting the current preset leaves the notes alone");

        // revert is staged the same way
        pd.revertPreset();
        renderBlocks(pd, 4, empty);
        expect(pd.activeVoiceCount() == 0,
               "switch: a revert drops the voices too");
    }

    // revert: back to the values the bank was built with, which is not the
    // same as reloading the slot once it has been left and re-entered
    {
        ParameterSet parameters;
        Parameter* a = parameters.add("a", "A", 0.0f, 1.0f, 0.25f);
        PresetBank bank(parameters);
        bank.add("one");          // slot 0 is registered holding 0.25
        a->set(0.9f);
        bank.add("two");          // slot 1 is registered holding 0.9
        bank.loadCurrent();       // as an instrument does after registering

        expect(a->get() == 0.25f, "revert: the bank starts on slot 0's values");
        a->set(0.5f);             // edit slot 0
        expect(bank.revertCurrent() && a->get() == 0.25f,
               "revert: an edit to the current slot is undone");

        // the case loadCurrent() cannot serve: leaving stores the edits, so
        // the slot's own copy becomes the edited sound
        a->set(0.7f);
        bank.select(1);
        bank.select(0);
        expect(a->get() == 0.7f, "revert: leaving and returning keeps the edits");
        bank.loadCurrent();
        expect(a->get() == 0.7f, "revert: reloading the slot cannot undo them");
        expect(bank.revertCurrent() && a->get() == 0.25f,
               "revert: the startup values are restored anyway");

        // and the slot stays reverted afterwards, not just the live value
        a->set(0.4f);
        bank.select(1);
        bank.select(0);
        expect(a->get() == 0.4f, "revert: editing after a revert behaves normally");
        bank.revertCurrent();
        bank.select(1);
        bank.select(0);
        expect(a->get() == 0.25f, "revert: the reverted slot stays reverted");

        expect(bank.revision() != 0, "revert: bulk loads bump the revision");
    }

    // a revert reloads the slot it is already on, so a display keyed on the
    // slot number alone would miss it and report ~100 parameter lines
    {
        PdSynthProcessor pd;
        ParameterMonitor monitor(pd);
        struct CountingDisplay : ParameterDisplay {
            int parameters = 0;
            int presets = 0;
            void showParameter(const DisplayLine&) override { parameters++; }
            void showPreset(int, const std::string&) override { presets++; }
        } display;
        monitor.addDisplay(&display);
        monitor.poll();

        pd.parameters().byId("volume")->set(0.3f);
        monitor.poll();
        display.parameters = 0;
        display.presets = 0;

        pd.presets()->revertCurrent();
        monitor.poll();
        expect(display.presets == 1 && display.parameters == 0,
               "monitor: a revert is reported as one preset line");
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
               && display.presets[0] == "2:" + pd.presets()->name(2),
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
        expect(bank->count() == pd_presets::kFactoryCount + PdSynthProcessor::kUserSlots,
               "pd: the bank is the factory group followed by 16 user slots");

        // the user group must be the tail of the bank, so that adding a
        // factory sound never renumbers a user slot
        const int firstUser = pd_presets::kFactoryCount;
        bool tailMatches = true;
        for(int i = 0; i < pd_presets::kUserCount; i++){
            tailMatches = tailMatches
                       && bank->name(firstUser + i) == pd_presets::kUser[i].name;
        }
        expect(tailMatches, "pd: the user slots sit at the end of the bank");

        // Every slot must load exactly what was generated for it — this is
        // what says the .vstpreset conversion reaches the parameters at
        // all. A preset saved before a parameter existed stops short of it,
        // and the rest must come from the defaults.
        // (the padding slots past the user presets are the init patch:
        // their "source" is no values at all, i.e. every one defaulted)
        const pd_presets::Preset padding{"", nullptr, 0};
        for(int slot = 0; slot < bank->count(); slot++){
            const pd_presets::Preset& source =
                (slot < firstUser)? pd_presets::kFactory[slot]
                : (slot - firstUser < pd_presets::kUserCount)? pd_presets::kUser[slot - firstUser]
                : padding;
            bank->select(slot);
            bool stored = true;
            bool defaulted = true;
            int index = 0;
            for(auto& parameter : pd.parameters()){
                if(index < source.count){
                    stored = stored
                        && parameter->get() == static_cast<float>(source.values[index]);
                }else{
                    defaulted = defaulted && parameter->get() == parameter->defaultValue();
                }
                index++;
            }
            expect(stored, ("pd: slot loads its converted values: "
                            + bank->name(slot)).c_str());
            expect(defaulted, ("pd: parameters newer than the preset take their "
                               "default: " + bank->name(slot)).c_str());
        }
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

    // MCP23017 panel, offline: encoder decoding, button debounce, pin rules
    {
        Mcp23017Panel panel;
        panel.addExpander(0x20);
        panel.addExpander(0x21);
        const int enc = panel.addEncoder({0, 0}, {0, 1});     // 0.A0 / 0.A1
        const int split = panel.addEncoder({0, 2}, {1, 0});   // A on chip 0, B on chip 1
        const int button = panel.addButton({0, 8});           // 0.B0
        expect(enc == 0 && split == 1 && button == 0, "panel: controls get channels in order");
        expect(panel.addButton({0, 7}) < 0, "panel: GPA7 is refused as an input");
        expect(panel.addLed({0, 7}) == 0, "panel: GPA7 is accepted for an LED");
        expect(panel.addButton({0, 8}) < 0, "panel: a pin cannot be used twice");
        expect(panel.addButton({2, 0}) < 0, "panel: undeclared expander is refused");
        expect(panel.addExpander(0x20) < 0, "panel: duplicate expander address is refused");

        Mcp23017Panel::Options options;
        options.quartersPerStep = 4;
        options.debounceMs = 5;
        panel.prepareOffline(options);

        // idle: everything pulled up
        uint16_t chip0 = 0xFFFF;
        int64_t t = 0;
        panel.processSnapshot(0, chip0, t);
        panel.processSnapshot(1, 0xFFFF, t);
        // encoder 0 on bits 0 (A) and 1 (B): one clockwise cycle from 11
        auto setAb = [&](int a, int b){
            chip0 = static_cast<uint16_t>((chip0 & ~0x3) | (a << 0) | (b << 1));
            panel.processSnapshot(0, chip0, ++t);
        };
        setAb(1, 0); setAb(0, 0); setAb(0, 1); setAb(1, 1);
        expect(panel.encoders().readDelta(enc) == +1, "panel: quadrature cycle is one step");
        setAb(0, 1); setAb(0, 0); setAb(1, 0); setAb(1, 1);
        expect(panel.encoders().readDelta(enc) == -1, "panel: reverse cycle is one step back");
        expect(panel.encoders().readDelta(enc) == 0, "panel: readDelta consumes the steps");

        // encoder 1 spans two chips: A = 0.A2, B = 1.A0
        uint16_t chip1 = 0xFFFF;
        auto setSplit = [&](int a, int b){
            chip0 = static_cast<uint16_t>((chip0 & ~0x4) | (a << 2));
            chip1 = static_cast<uint16_t>((chip1 & ~0x1) | b);
            panel.processSnapshot(0, chip0, ++t);
            panel.processSnapshot(1, chip1, t);
        };
        setSplit(1, 0); setSplit(0, 0); setSplit(0, 1); setSplit(1, 1);
        expect(panel.encoders().readDelta(split) == +1,
               "panel: an encoder split across expanders decodes");

        // button on bit 8: press, bounce, settle
        ButtonEvent event;
        while(panel.buttons().popEvent(event)){}
        t = 100;
        panel.processSnapshot(0, chip0 & ~0x100, t);       // press
        panel.processSnapshot(0, chip0, t + 1);            // bounce up
        panel.processSnapshot(0, chip0 & ~0x100, t + 2);   // bounce down
        panel.processSnapshot(0, chip0 & ~0x100, t + 10);  // settled pressed
        int edges = 0;
        bool lastPressed = false;
        while(panel.buttons().popEvent(event)){
            edges++;
            lastPressed = event.pressed;
        }
        expect(edges == 1 && lastPressed, "panel: a bouncing press is one edge");

        panel.processSnapshot(0, chip0, t + 20);           // release
        panel.processSnapshot(0, chip0 & ~0x100, t + 21);  // bounce
        panel.processSnapshot(0, chip0 & ~0x100, t + 22);  // contact stays down (glitch)
        panel.processSnapshot(0, chip0, t + 30);           // settled released
        edges = 0;
        bool sawRelease = false;
        while(panel.buttons().popEvent(event)){
            edges++;
            sawRelease |= !event.pressed;
        }
        expect(edges == 1 && sawRelease, "panel: a bouncing release is one edge");
        // a contact that settles inside the window is reported on a later
        // scan even though the port value no longer changes
        panel.processSnapshot(0, chip0 & ~0x100, t + 40);  // press
        panel.processSnapshot(0, chip0, t + 41);           // released within window
        panel.processSnapshot(0, chip0, t + 50);           // same value, window over
        bool pressed = false, released = false;
        while(panel.buttons().popEvent(event)){
            (event.pressed? pressed : released) = true;
        }
        expect(pressed && released, "panel: debounce settles on an unchanged port");
    }

    // panel config file
    {
        std::istringstream text(
            "# test panel\n"
            "bus /dev/i2c-3\n"
            "expander 0x20\n"
            "expander 0x21 int 17  # second\n"
            "interrupt 4\n"
            "encoder-step 0.02\n"
            "encoder row1.1 0.A0 0.A1\n"
            "button  sus    1.b3\n"
            "led     dcw    1.B7 active-low\n");
        PanelConfig config;
        std::string error;
        const bool ok = parsePanelConfig(text, config, error);
        expect(ok && config.options.busPath == "/dev/i2c-3"
               && config.expanders.size() == 2 && config.expanders[1].address == 0x21
               && config.options.interruptLine == 4
               && config.expanders[0].interruptLine == -1
               && config.expanders[1].interruptLine == 17
               && std::abs(config.encoderStep - 0.02f) < 1e-6f,
               "panel config: settings are read");
        expect(ok && config.encoders.size() == 1 && config.encoders[0].role == "row1.1"
               && config.encoders[0].b.pin == 1
               && config.buttons.size() == 1 && config.buttons[0].pin.chip == 1
               && config.buttons[0].pin.pin == 11
               && config.leds.size() == 1 && config.leds[0].activeLow
               && config.leds[0].pin.pin == 15,
               "panel config: controls and pins are read");

        std::istringstream bad("expander 0x20\nencoder row1.1 0.A0 0.C1\n");
        PanelConfig badConfig;
        error.clear();
        expect(!parsePanelConfig(bad, badConfig, error) && error.rfind("line 2", 0) == 0,
               "panel config: a bad pin is reported with its line");
    }

    // panel framework: generic roles, abs target, discrete stepping, blink
    {
        ParameterSet params;
        Parameter* level = params.add("level", "", 0, 1, 0.5f);       // continuous
        Parameter* mode = params.add("mode", "", 0, 1, 0.0f);         // 4 options
        mode->setSteps(4);

        struct FakeEncoders : RelativeControlInput {
            int pending[4] = {};
            const char* name() const override { return "fake"; }
            int numChannels() const override { return 4; }
            int readDelta(int ch) override { const int d = pending[ch]; pending[ch] = 0; return d; }
        } encoders;
        struct FakeLeds : LedOutput {
            bool on[8] = {};
            int flushes = 0;
            int numChannels() const override { return 8; }
            void setLed(int ch, bool value) override { on[ch] = value; }
            void flush() override { flushes++; }
        } leds;
        struct Blinker : PanelLogic::Module {
            PanelLogic::LedFn led(const std::string& role) override {
                if(role != "blinky") return {};
                return []{ return LedState::Blink; };
            }
            std::string unavailable() const override { return "blinker is a test"; }
        };

        PanelLogic logic(params);
        logic.addModule(std::make_unique<Blinker>());
        logic.setStepSize(0.01f);
        expect(logic.attachEncoder("param:level", &encoders, 0)
               && logic.attachEncoder("param:mode", &encoders, 1)
               && logic.attachEncoder("abs", &encoders, 2)
               && !logic.attachEncoder("param:nope", &encoders, 3)
               && !logic.attachEncoder("bogus", &encoders, 3),
               "panel logic: encoder roles resolve (and unknown ones do not)");
        auto up = logic.button("abs.up");
        auto down = logic.button("abs.down");
        auto cycle = logic.button("param:mode");
        expect(up && down && cycle && !logic.button("bogus"), "panel logic: button roles resolve");
        expect(logic.attachLed("param:mode=2", &leds, 0) && logic.attachLed("blinky", &leds, 1)
               && !logic.attachLed("param:mode=x", &leds, 2),
               "panel logic: LED roles resolve");
        expect(logic.warnings().size() == 1 && logic.warnings()[0] == "blinker is a test",
               "panel logic: a module that cannot work is reported once");

        encoders.pending[0] = +3;
        logic.poll();
        expect(std::abs(level->getNormalized() - 0.53f) < 1e-5f && logic.target() == level,
               "panel logic: a continuous parameter moves by the step size");
        up(true); up(false);
        expect(std::abs(level->getNormalized() - 0.54f) < 1e-5f,
               "panel logic: abs.up nudges what was touched last");
        encoders.pending[1] = +2;
        logic.poll();
        expect(mode->option() == 2 && logic.target() == mode && leds.on[0],
               "panel logic: a discrete parameter moves one option per detent");
        down(true);
        logic.poll();
        expect(mode->option() == 1 && !leds.on[0], "panel logic: abs.down steps the option back");
        encoders.pending[2] = +5;   // abs encoder, clamped at the last option
        logic.poll();
        expect(mode->option() == 3, "panel logic: the abs encoder clamps at the last option");
        cycle(true);
        expect(mode->option() == 0, "panel logic: a param button wraps to the first option");

        // blink: on for kBlinkTicks polls, then off
        bool sawOn = false, sawOff = false;
        const int flushesBefore = leds.flushes;
        for(int i = 0; i < 2 * PanelLogic::kBlinkTicks; i++){
            logic.poll();
            (leds.on[1]? sawOn : sawOff) = true;
        }
        expect(sawOn && sawOff, "panel logic: a blinking LED blinks");
        expect(leds.flushes - flushesBefore <= 2, "panel logic: outputs are flushed only on change");
    }

    // presets: compare (A/B) and the edited flag
    {
        ParameterSet params;
        Parameter* a = params.add("a", "", 0, 1, 0.2f);
        Parameter* bend = params.add("bend", "", 0, 1, 0.5f);
        bend->setTransient(true);
        PresetBank bank(params);
        bank.add("one");
        a->set(0.7f);
        bank.add("two");
        bank.loadCurrent();
        expect(!bank.isEdited(), "compare: a freshly loaded tone is not edited");
        bend->set(0.9f);
        expect(!bank.isEdited(), "compare: a transient parameter is not an edit");
        a->set(0.4f);
        expect(bank.isEdited(), "compare: an edit is noticed");
        bank.toggleCompare();
        expect(bank.comparing() && a->get() == 0.2f && !bank.isEdited(),
               "compare: on plays the original");
        bank.toggleCompare();
        expect(!bank.comparing() && a->get() == 0.4f, "compare: off brings the edit back");
        bank.toggleCompare();
        bank.select(1);
        bank.select(0);
        expect(!bank.comparing() && a->get() == 0.4f,
               "compare: switching tones while comparing keeps the edit");
        bank.toggleCompare();
        bank.revertCurrent();
        expect(!bank.comparing() && a->get() == 0.2f && !bank.isEdited(),
               "compare: initialize ends the comparison and drops the edit");
    }

#ifdef RTSYNTH_HAVE_PD
    // the pd panel modules, against the real instrument
    {
        PdSynthProcessor pd;
        ParameterSet& params = pd.parameters();
        PresetBank* bank = pd.presets();

        struct FakeEncoders : RelativeControlInput {
            int pending[32] = {};
            const char* name() const override { return "fake"; }
            int numChannels() const override { return 32; }
            int readDelta(int ch) override { const int d = pending[ch]; pending[ch] = 0; return d; }
        } enc;
        struct FakeLeds : LedOutput {
            bool on[128] = {};
            int numChannels() const override { return 128; }
            void setLed(int ch, bool value) override { on[ch] = value; }
        } leds;

        std::vector<int> programs;
        int compares = 0, initializes = 0;
        PdToneSelect::Host host;
        host.programChange = [&](int program){ programs.push_back(program); };
        host.toggleCompare = [&]{ compares++; };
        host.initialize = [&]{ initializes++; };

        PanelLogic logic(params);
        logic.addModule(std::make_unique<PdLineEditor>(logic, params));
        logic.addModule(std::make_unique<PdLineSelect>(logic, params));
        logic.addModule(std::make_unique<PdToneSelect>(bank, host));
        logic.addModule(std::make_unique<PdDetuneVibrato>(logic, params));

        // encoders 0-7 = row 1, 8-15 = row 2, 16-18 = dv.1-3, 19 = abs
        for(int row = 1; row <= 2; row++){
            for(int k = 1; k <= 8; k++){
                logic.attachEncoder("row" + std::to_string(row) + "." + std::to_string(k),
                                    &enc, (row - 1) * 8 + k - 1);
            }
        }
        for(int i = 1; i <= 3; i++) logic.attachEncoder("dv." + std::to_string(i), &enc, 15 + i);
        logic.attachEncoder("abs", &enc, 19);
        auto press = [&](const std::string& role){
            auto fn = logic.button(role);
            expect(static_cast<bool>(fn), ("pd panel: button role " + role).c_str());
            if(fn){ fn(true); fn(false); }
        };
        // LEDs: 0-2 row1 dco/dcw/dca, 3-4 rate/level, 10+K row1 stepK,
        // 80+K wave1 K, 90+K wave2 K, 40-43 line.1/2/1'/2',
        // 50 preset 51 user 52 page 53 compare, 60+N toneN, 70 detune 71 vib
        const char* row1[] = {"dco", "dcw", "dca", "rate", "level"};
        for(int i = 0; i < 5; i++) logic.attachLed(std::string("row1.") + row1[i], &leds, i);
        for(int k = 1; k <= 8; k++) logic.attachLed("row1.step" + std::to_string(k), &leds, 10 + k);
        for(int k = 1; k <= 9; k++){
            logic.attachLed("row1.wave1." + std::to_string(k), &leds, 80 + k);
            logic.attachLed("row1.wave2." + std::to_string(k), &leds, 90 + k);
        }
        logic.attachLed("row1.kf", &leds, 100);
        const char* lines[] = {"line.1", "line.2", "line.1'", "line.2'"};
        for(int i = 0; i < 4; i++) logic.attachLed(lines[i], &leds, 40 + i);
        const char* tone[] = {"preset", "user", "page", "compare"};
        for(int i = 0; i < 4; i++) logic.attachLed(tone[i], &leds, 50 + i);
        for(int n = 1; n <= 8; n++) logic.attachLed("tone" + std::to_string(n), &leds, 60 + n);
        logic.attachLed("detune", &leds, 70);
        logic.attachLed("vib", &leds, 71);
        expect(!logic.attachLed("row1.sus1", &leds, 99) && !logic.attachLed("row3.dco", &leds, 99)
               && !logic.attachLed("tone9", &leds, 99),
               "pd panel: unknown LED roles are refused");
        logic.poll();

        // --- line editor ----------------------------------------------------
        Parameter* rate3 = params.byId("line1_dco_rate3");
        const float rateBefore = rate3->getNormalized();
        enc.pending[2] = +5;
        logic.poll();
        expect(std::abs(rate3->getNormalized() - std::min(1.0f, rateBefore + 0.05f)) < 1e-4f
               && leds.on[0] && leds.on[3],
               "pd panel: row 1 starts on DCO rate");
        press("row1.eg");
        press("row1.field");
        Parameter* l1Level2 = params.byId("line1_dcw_level2");
        Parameter* l2Rate2 = params.byId("line2_dco_rate2");
        l1Level2->setNormalized(0.5f);
        l2Rate2->setNormalized(0.5f);
        enc.pending[1] = -10;
        enc.pending[9] = +10;
        logic.poll();
        expect(std::abs(l1Level2->getNormalized() - 0.40f) < 1e-4f
               && std::abs(l2Rate2->getNormalized() - 0.60f) < 1e-4f,
               "pd panel: each row keeps its own DCx and rate/level");
        expect(!leds.on[0] && leds.on[1] && leds.on[4] && !leds.on[3],
               "pd panel: the row's LEDs follow its selection");
        press("row1.eg"); press("row1.eg");
        expect(logic.target() == l2Rate2, "pd panel: selection buttons leave the target alone");

        // sustain / end on the shared step LEDs (row 1 is on DCO now)
        Parameter* sus = params.byId("line1_dco_sustain");
        Parameter* end = params.byId("line1_dco_end");
        sus->setOption(0);   // Off
        end->setOption(3);   // step 5
        logic.poll();
        bool onlyEnd = leds.on[15];
        for(int k = 1; k <= 8; k++) if(k != 5) onlyEnd = onlyEnd && !leds.on[10 + k];
        expect(onlyEnd, "pd panel: sustain Off lights only the end step");
        press("row1.sus"); press("row1.sus"); press("row1.sus");
        press("row1.end");
        logic.poll();
        expect(sus->option() == 3 && end->option() == 4 && leds.on[13] && leds.on[16]
               && !leds.on[15],
               "pd panel: sus / end step on, both points lit");
        auto litSteps = [&]{
            std::vector<int> lit;
            for(int k = 1; k <= 8; k++) if(leds.on[10 + k]) lit.push_back(k);
            return lit;
        };
        sus->setOption(6);   // step 6 = end (step 6): no sustain before the end
        logic.poll();
        expect(litSteps() == std::vector<int>{6},
               "pd panel: a sustain at the end point shows only the end");
        sus->setOption(7);   // step 7, after the end
        logic.poll();
        expect(litSteps() == std::vector<int>{6},
               "pd panel: a sustain after the end point shows only the end");
        sus->setOption(5);   // step 5, before the end
        logic.poll();
        expect(litSteps() == (std::vector<int>{5, 6}),
               "pd panel: a sustain before the end lights both (lower = sustain)");
        sus->setOption(3);
        logic.poll();
        for(int i = 0; i < 5; i++) press("row1.sus");
        expect(sus->option() == 0, "pd panel: sustain wraps 7 -> Off");
        press("abs.down");
        expect(sus->option() == 0, "pd panel: abs.down after sus stays at Off (no wrap)");
        press("row1.sus");
        press("abs.up");
        expect(sus->option() == 2, "pd panel: abs steps what sus last changed");

        // waveforms: one LED per waveform
        Parameter* wave1 = params.byId("line1_wave1");
        Parameter* wave2 = params.byId("line1_wave2");
        auto litWaves = [&](int base){
            int lit = 0, count = 0;
            for(int k = 1; k <= 9; k++) if(leds.on[base + k]){ lit = k; count++; }
            return (count == 1)? lit : (count == 0)? 0 : -1;
        };
        wave1->setOption(5);   // waveform 6
        wave2->setOption(0);   // Off
        logic.poll();
        expect(litWaves(80) == 6 && litWaves(90) == 0,
               "pd panel: waveform 6 lights LED 6 alone, wave2 Off lights none");
        press("row1.wave2"); press("row1.wave2"); press("row1.wave2");
        logic.poll();
        expect(wave2->option() == 3 && litWaves(90) == 3, "pd panel: wave2 steps to waveform 3");
        press("row1.wave1"); press("row1.wave1");
        logic.poll();
        expect(wave1->option() == 7 && litWaves(80) == 8, "pd panel: waveform 8 lights LED 8");
        press("row1.wave1");
        logic.poll();
        expect(wave1->option() == 0 && litWaves(80) == 1, "pd panel: wave1 wraps 8 -> 1");
        for(int i = 0; i < 6; i++) press("row1.wave2");
        logic.poll();
        expect(wave2->option() == 0 && litWaves(90) == 0, "pd panel: wave2 wraps 8 -> Off");

        // key follow: wave1 + wave2 together toggle key follow mode
        Parameter* kfDcw = params.byId("line1_dcw_key_follow");
        Parameter* kfDca = params.byId("line1_dca_key_follow");
        kfDcw->setOption(0);
        kfDca->setOption(4);
        wave1->setOption(2);
        wave2->setOption(5);
        auto w1 = logic.button("row1.wave1");
        auto w2 = logic.button("row1.wave2");
        w1(true);
        expect(wave1->option() == 2, "pd panel: a wave button does nothing until released");
        w2(true);            // chord
        w1(false);
        w2(false);
        logic.poll();
        expect(wave1->option() == 2 && wave2->option() == 5,
               "pd panel: a chord does not step the waveforms");
        expect(leds.on[100] && litWaves(80) == 0 && litWaves(90) == 4,
               "pd panel: key follow mode: KF LED on, DCW 0 = dark, DCA 4 = LED 4");
        expect(logic.target() == kfDcw, "pd panel: entering key follow puts it on the LCD");
        for(int i = 0; i < 9; i++) press("row1.wave1");
        press("row1.wave2");
        logic.poll();
        expect(kfDcw->option() == 9 && kfDca->option() == 5 && litWaves(80) == 9 && litWaves(90) == 5,
               "pd panel: in key follow mode the wave buttons step 0..9 (9 = LED 9)");
        press("row1.wave1");
        logic.poll();
        expect(kfDcw->option() == 0 && litWaves(80) == 0, "pd panel: key follow wraps 9 -> 0");
        expect(wave1->option() == 2 && wave2->option() == 5,
               "pd panel: the waveforms are untouched in key follow mode");
        w2(true); w1(true); w2(false); w1(false);   // chord the other way round
        logic.poll();
        expect(!leds.on[100] && litWaves(80) == 3 && litWaves(90) == 5,
               "pd panel: a second chord returns to the waveforms");
        press("row1.keyfollow");
        logic.poll();
        expect(leds.on[100], "pd panel: a keyfollow button toggles the same mode");
        press("row1.keyfollow");

        // line select LEDs
        Parameter* lineSelect = params.byId("line_select");
        lineSelect->setOption(1);
        press("line");
        logic.poll();
        expect(lineSelect->option() == 2 && leds.on[40] && !leds.on[41] && leds.on[42] && !leds.on[43],
               "pd panel: 1+1' lights 1 and 1'");
        press("line");
        logic.poll();
        expect(leds.on[40] && !leds.on[42] && leds.on[43], "pd panel: 1+2' lights 1 and 2'");

        // --- tone select: every button switches at once ---------------------
        // (the fake host only records; the test lands each switch itself,
        // the way the audio thread would)
        const int userBase = bank->count() - 16;
        auto land = [&]{ if(!programs.empty()) bank->select(programs.back()); logic.poll(); };
        bank->select(2);   // preset 3
        logic.poll();
        expect(leds.on[50] && !leds.on[51] && !leds.on[52] && leds.on[63],
               "pd panel: preset 3 lights PRESET and tone 3");
        press("page");
        expect(programs.size() == 1 && programs[0] == 10,
               "pd panel: page on preset 3 goes straight to preset 11");
        land();
        expect(leds.on[50] && leds.on[52] && leds.on[63], "pd panel: preset 11 = PRESET, PAGE, tone 3");
        press("user");
        expect(programs.back() == userBase + 10, "pd panel: user goes to user 11");
        press("page");   // before the switch lands: acts on the requested tone
        expect(programs.back() == userBase + 2,
               "pd panel: a second press before the switch lands builds on the first");
        land();
        expect(leds.on[51] && !leds.on[52] && leds.on[63], "pd panel: user 3 = USER, tone 3");
        press("tone5");
        land();
        expect(programs.back() == userBase + 4 && leds.on[65] && !leds.on[63],
               "pd panel: tone 5 on the user bank");
        const size_t sent = programs.size();
        press("tone5");
        expect(programs.size() == sent, "pd panel: the current tone is not re-sent");
        press("preset");
        land();
        expect(programs.back() == 4 && leds.on[50] && leds.on[65], "pd panel: preset goes to preset 5");
        bank->select(userBase + 9);   // e.g. a MIDI Program Change
        logic.poll();
        expect(leds.on[51] && leds.on[52] && leds.on[62],
               "pd panel: the LEDs follow a tone changed elsewhere (user 10)");
        bank->select(1);

        // compare / initialize
        press("compare");
        expect(compares == 0, "pd panel: compare does nothing on an unedited tone");
        params.byId("line1_dca_rate1")->setNormalized(0.123f);
        logic.poll();
        expect(leds.on[53], "pd panel: an edit lights COMPARE");
        press("compare");
        expect(compares == 1, "pd panel: compare is sent while edited");
        bank->toggleCompare();   // what the audio thread does with it
        bool compareBlinks = false;
        for(int i = 0; i < 2 * PanelLogic::kBlinkTicks; i++){
            logic.poll();
            compareBlinks |= !leds.on[53];
        }
        expect(compareBlinks, "pd panel: COMPARE blinks while the original plays");
        press("compare");
        press("initialize");
        expect(compares == 2 && initializes == 1, "pd panel: compare back, then initialize");
        bank->toggleCompare();

        // detune / vibrato
        Parameter* oct = params.byId("detune_octave");
        oct->setOption(3);   // 0 oct
        enc.pending[16] = +1;
        logic.poll();
        expect(oct->option() == 4 && leds.on[70] && !leds.on[71],
               "pd panel: dv.1 steps the detune octave");
        press("detune-vib");
        enc.pending[16] = +1;
        logic.poll();
        expect(oct->option() == 4 && leds.on[71], "pd panel: in vib mode dv.1 leaves detune alone");
        press("vib.on");
        bool vibWarned = false;
        for(const std::string& w : logic.warnings()) vibWarned |= w.rfind("vibrato:", 0) == 0;
        expect(vibWarned && logic.warnings().size() == 1,
               "pd panel: only the missing vibrato is reported");
    }
#endif

    // the shipped panel config parses and every role in it is understood
    {
        PanelConfig config;
        std::string error;
        bool ok = loadPanelConfig(RTSYNTH_SOURCE_DIR "/panels/pd_panel.conf", config, error);
        Mcp23017Panel hw;
        SineSynthProcessor sine;   // roles resolve without pd; they just do nothing
        PanelLogic logic(sine.parameters());
        logic.addModule(std::make_unique<PdLineEditor>(logic, sine.parameters()));
        logic.addModule(std::make_unique<PdLineSelect>(logic, sine.parameters()));
        logic.addModule(std::make_unique<PdToneSelect>(nullptr, PdToneSelect::Host{}));
        logic.addModule(std::make_unique<PdDetuneVibrato>(logic, sine.parameters()));
        for(const auto& e : config.expanders) ok = ok && hw.addExpander(e.address, e.interruptLine) >= 0;
        for(const auto& e : config.encoders){
            const int channel = hw.addEncoder(e.a, e.b);
            ok = ok && channel >= 0 && logic.attachEncoder(e.role, &hw.encoders(), channel);
            if(!ok){ std::printf("       encoder line %d: %s\n", e.line, hw.lastError().c_str()); break; }
        }
        for(const auto& b : config.buttons){
            ok = ok && hw.addButton(b.pin) >= 0 && static_cast<bool>(logic.button(b.role));
            if(!ok){ std::printf("       button line %d: %s\n", b.line, hw.lastError().c_str()); break; }
        }
        ShiftRegisterLeds chain;
        chain.setChainLength(config.shiftChips);
        for(const auto& l : config.leds){
            const int channel = l.onShiftRegister? l.shiftChannel : hw.addLed(l.pin, l.activeLow);
            ok = ok && channel >= 0
                 && logic.attachLed(l.role, l.onShiftRegister? static_cast<LedOutput*>(&chain)
                                                             : &hw.leds(), channel);
            if(!ok){ std::printf("       led line %d\n", l.line); break; }
        }
        expect(ok && config.encoders.size() == 19 && config.buttons.size() == 29
               && config.leds.size() == 87 && config.expanders.size() == 5
               && config.shiftChips == 10,
               "panels/pd_panel.conf: 19 encoders, 29 buttons, 87 LEDs, all valid");
        if(!error.empty()){
            std::printf("       %s\n", error.c_str());
        }
    }

    // 74HC595 chain: frame order and polarity, offline
    {
        ShiftRegisterLeds chain;
        chain.setChainLength(3);
        chain.setActiveLow(17, true);       // sr2.1
        chain.setLed(0, true);              // sr0.QA
        chain.setLed(15, true);             // sr1.QH
        const std::vector<uint8_t> frame = chain.frame();
        expect(frame.size() == 3 && frame[0] == 0x02 && frame[1] == 0x80 && frame[2] == 0x01,
               "595 chain: last chip first, QA = bit 0, active-low inverted");
        chain.setLed(0, false);
        chain.setLed(17, true);
        expect(chain.frame()[0] == 0x00 && chain.frame()[2] == 0x00,
               "595 chain: LEDs switch off, an active-low LED on drives low");

        std::istringstream text("expander 0x20\nshift-register /dev/spidev0.1 2\n"
                                "led a sr1.7\nled b 0.B7\n");
        PanelConfig config;
        std::string error;
        expect(parsePanelConfig(text, config, error) && config.leds[0].onShiftRegister
               && config.leds[0].shiftChannel == 15 && !config.leds[1].onShiftRegister,
               "panel config: sr pins and expander pins for LEDs");
        std::istringstream beyond("expander 0x20\nshift-register /dev/spidev0.1 1\nled a sr1.0\n");
        PanelConfig c2;
        expect(!parsePanelConfig(beyond, c2, error) && error.rfind("line 3", 0) == 0,
               "panel config: an LED past the end of the chain is refused");
        std::istringstream twice("expander 0x20\nshift-register x 1\nled a sr0.0\nled b sr0.0\n");
        PanelConfig c3;
        expect(!parsePanelConfig(twice, c3, error), "panel config: an sr pin used twice is refused");
    }

    // ControlLoop drains buttons on its own thread's poll
    {
        struct FakeButtons : ButtonInput {
            const char* name() const override { return "fake"; }
            int numChannels() const override { return 2; }
            void press(int ch, bool down){ publish(ch, down); }
        } buttons;
        ControlLoop loop;
        int seen = 0;
        loop.addButtons(&buttons, [&](int ch, bool pressed){
            if(ch == 1 && pressed) seen++;
        });
        buttons.press(1, true);
        buttons.press(1, false);
        buttons.press(1, true);
        expect(loop.hasWork(), "control loop: buttons count as work");
        loop.pollOnce();
        expect(seen == 2, "control loop: queued button edges reach the handler");
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
