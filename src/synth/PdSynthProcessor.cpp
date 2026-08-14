#include <algorithm>
#include <cmath>
#include <cstdio>

#include "PdSynthProcessor.hpp"

namespace rtsynth {

using namespace Steinberg::Vst;

namespace {

constexpr uint8_t kCcAllSoundOff = 120;
constexpr uint8_t kCcAllNotesOff = 123;

// normalized encodings of the discrete init-patch choices (see const.h):
// sustain point option 1 of 8 -> the EG sustains at step 1
constexpr double kSustainAtStep1 = 1.0 / (kNumEgSustainPointOptions - 1);

// Mirrors pd's controller.cpp getMidiControllerAssignment(): in the VST3
// plugin, the *host* consults this table to route raw MIDI CC into
// normalized parameter changes before the processor ever sees them, so
// processor.cpp itself contains no CC handling. Standalone there is no
// host doing that translation, so PdSynthProcessor must reimplement the
// same mapping directly (paramIdForCc). Kept in sync with pd's
// controller.cpp; if that table changes there, mirror the change here.
constexpr uint8_t kCcEditLineController = 3;   // 0-63 = line 1, 64-127 = line 2
constexpr uint8_t kCcVolume = 7;
constexpr uint8_t kCcLineSelect = 9;
constexpr uint8_t kCcDetuneOctave = 85;
constexpr uint8_t kCcDetuneNote = 86;
constexpr uint8_t kCcDetuneFine = 87;
constexpr uint8_t kCcWaveformFirst = 89;
constexpr uint8_t kCcWaveformSecond = 90;
constexpr uint8_t kCcMonoModeOn = 126;
constexpr uint8_t kCcPolyModeOn = 127;
constexpr uint8_t kEgCcBlockFirst[3] = {
    14,   // DCO EG: CC 14-30
    46,   // DCW EG: CC 46-62
    102,  // DCA EG: CC 102-118
};

const char* const kWaveformNames[] = {
    "Saw Tooth", "Square", "Pulse", "Double Sine",
    "Saw Pulse", "Resonance I Saw", "Resonance II Tri", "Resonance III Trap",
};

const char* const kEgNames[3] = {"DCO", "DCW", "DCA"};

// prefix + number + suffix, built by appending. Written this way because
// the obvious `"L" + std::to_string(n)` prepends to a temporary string,
// and GCC 12 then loses track of that string's length and reports a bogus
// -Wrestrict overlap warning for the memcpy the insert compiles to.
std::string concat(const char* prefix, int number, const char* suffix = ""){
    std::string text = prefix;
    text += std::to_string(number);
    text += suffix;
    return text;
}

// Offset within a line parameter block addressed by `cc`, or -1 if `cc`
// is not one of the EG controller ranges above.
int lineParamOffsetForCc(uint8_t cc){
    for(int egIndex = 0; egIndex < 3; egIndex++){
        const uint8_t first = kEgCcBlockFirst[egIndex];
        if(first <= cc && cc < first + kLineParamEgBlockSize){
            return kLineParamEgBegin + egIndex * kLineParamEgBlockSize + (cc - first);
        }
    }
    return -1;
}

// Normalized value that a control sends for option `index` of a discrete
// parameter — the index/(numOptions-1) convention pd's decodeOptionIndex()
// is built to reproduce exactly.
constexpr double optionValue(int index, int numOptions){
    return (numOptions > 1)? index / static_cast<double>(numOptions - 1) : 0.0;
}

constexpr int lineParam(int line, int offset){
    return kParamLine1Begin + line * kNumLineParams + offset;
}

constexpr int egParam(int line, int egIndex, int sub){
    return lineParam(line, kLineParamEgBegin + egIndex * kLineParamEgBlockSize + sub);
}

constexpr int kEgDco = 0, kEgDcw = 1, kEgDca = 2;

// pd's EG rate is a *time*, not a slope: EG::rateToSample() spends
// 5 * (1 - rate) seconds moving to the step's level whatever the distance.
// Presets and the display are both easier to read in milliseconds.
constexpr double rateForSeconds(double seconds){ return 1.0 - seconds / 5.0; }
double secondsForRate(double rate){ return 5.0 * (1.0 - rate); }

}  // namespace

PdSynthProcessor::PdSynthProcessor(){
    heldNotes_.reserve(256);  // mono mode bookkeeping; never grows in process()
    registerParameters();
    registerFactoryPresets();
    for(int paramId = 0; paramId < kNumPdParams; paramId++){
        appliedValues_[paramId] = paramHandles_[paramId]->get();
        applyParameter(paramId, appliedValues_[paramId]);
    }
}

std::string PdSynthProcessor::lineParamId(int line, int offset){
    std::string id = concat("line", line + 1, "_");
    if(offset == kLineParamWaveformFirst){
        return id + "wave1";
    }
    if(offset == kLineParamWaveformSecond){
        return id + "wave2";
    }
    static const char* egNames[] = {"dco", "dcw", "dca"};
    const int egOffset = offset - kLineParamEgBegin;
    id += egNames[egOffset / kLineParamEgBlockSize];
    id += "_";
    const int sub = egOffset % kLineParamEgBlockSize;
    if(sub < kNumEgRateParams){
        return id + "rate" + std::to_string(sub + 1);
    }
    if(sub < kNumEgRateParams + kNumEgLevelParams){
        return id + "level" + std::to_string(sub - kNumEgRateParams + 1);
    }
    return id + ((sub == kEgParamSustainPoint)? "sustain" : "end");
}

std::string PdSynthProcessor::paramIdString(int paramId){
    switch(paramId){
        case kParamPitchBend:    return "pitch_bend";
        case kParamVolume:       return "volume";
        case kParamLineSelect:   return "line_select";
        case kParamMonoPoly:     return "mono";
        case kParamDetuneOctave: return "detune_octave";
        case kParamDetuneNote:   return "detune_note";
        case kParamDetuneFine:   return "detune_fine";
        case kParamCcEditLine:   return "cc_edit_line";
        case kParamMonoTrigger:  return "mono_trigger";
        case kParamPolyTrigger:  return "poly_trigger";
        default:                 break;
    }
    if(kParamLine1Begin <= paramId && paramId < kParamCcEditLine){
        const int rel = paramId - kParamLine1Begin;
        return lineParamId(rel / kNumLineParams, rel % kNumLineParams);
    }
    // A parameter appended to pd's ParamId enum that this host does not
    // know about yet. It still gets registered, so every id in [0,
    // kNumParams) has a handle and updating the submodule can never leave
    // a hole for the constructor and syncParameters() to walk into.
    return concat("pd_param", paramId);
}

std::string PdSynthProcessor::paramName(int paramId){
    switch(paramId){
        case kParamPitchBend:    return "Pitch Bend";
        case kParamVolume:       return "Volume";
        case kParamLineSelect:   return "Line Select";
        case kParamMonoPoly:     return "Mono/Poly";
        case kParamDetuneOctave: return "Detune Octave";
        case kParamDetuneNote:   return "Detune Note";
        case kParamDetuneFine:   return "Detune Fine";
        case kParamCcEditLine:   return "CC Edit Line";
        case kParamMonoTrigger:  return "Mono Trigger";
        case kParamPolyTrigger:  return "Poly Trigger";
        default:                 break;
    }
    if(paramId < kParamLine1Begin || paramId >= kParamCcEditLine){
        return paramIdString(paramId);
    }

    const int rel = paramId - kParamLine1Begin;
    const int offset = rel % kNumLineParams;
    std::string name = concat("L", rel / kNumLineParams + 1, " ");
    if(offset == kLineParamWaveformFirst){
        return name + "Wave 1st";
    }
    if(offset == kLineParamWaveformSecond){
        return name + "Wave 2nd";
    }
    const int egOffset = offset - kLineParamEgBegin;
    name += kEgNames[egOffset / kLineParamEgBlockSize];
    const int sub = egOffset % kLineParamEgBlockSize;
    if(sub < kNumEgRateParams){
        return name + " Rate " + std::to_string(sub + 1);
    }
    if(sub < kEgParamSustainPoint){
        return name + " Level " + std::to_string(sub - kEgParamLevel0 + 1);
    }
    return name + ((sub == kEgParamSustainPoint)? " Sustain Point" : " End Point");
}

// Defaults double as a small init patch: sawtooth on line 1, fast-attack
// DCA sustaining at full level, and a DCW sweep for the classic PD "filter"
// motion. (The plugin's processor defaults are all zero and thus silent —
// there the editor/controller supplies working values; standalone we must.)
double PdSynthProcessor::defaultParamValue(int paramId){
    switch(paramId){
        case kParamPitchBend:
        case kParamVolume:
        case kParamDetuneOctave:  // signed parameters center on 0.5
        case kParamDetuneNote:
        case kParamDetuneFine:
            return 0.5;
        default:
            break;
    }

    if(kParamLine1Begin <= paramId && paramId < kParamCcEditLine){
        const int offset = (paramId - kParamLine1Begin) % kNumLineParams;
        if(offset < kLineParamEgBegin){
            return 0.0;  // waveform 1 = sawtooth, waveform 2 = off
        }
        const int egOffset = offset - kLineParamEgBegin;
        const int egIndex = egOffset / kLineParamEgBlockSize;   // 0 dco, 1 dcw, 2 dca
        const int sub = egOffset % kLineParamEgBlockSize;
        if(egIndex >= 1){  // DCW and DCA share the same envelope shape
            switch(sub){
                case kEgParamRate0:          return 1.0;   // fast attack
                case kEgParamRate0 + 1:      return 0.9;   // ~0.5 s release
                case kEgParamLevel0:         return (egIndex == 1)? 0.8 : 1.0;
                case kEgParamSustainPoint:   return kSustainAtStep1;
                default:                     break;        // end point stays at step 2
            }
        }
    }
    return 0.0;
}

// Registers one Parameter per entry of pd's ParamId enum, driven by the
// enum's size rather than by a hand-written list. That is deliberate:
// bumping the external/pd submodule can append parameters (kParamMonoTrigger
// and kParamPolyTrigger arrived that way), and a hand-written list silently
// leaves those handles null for the constructor and syncParameters() to
// dereference.
void PdSynthProcessor::registerParameters(){
    for(int paramId = 0; paramId < kNumPdParams; paramId++){
        paramHandles_[paramId] = parameters_.add(
            paramIdString(paramId), paramName(paramId), 0.0f, 1.0f,
            static_cast<float>(defaultParamValue(paramId)));
    }
}

// The factory bank. Slot 0 is the init patch (the parameter defaults); the
// rest are sparse overrides on top of it, which keeps a patch readable as
// "what makes this sound different" instead of 118 numbers.
void PdSynthProcessor::registerFactoryPresets(){
    struct Override {
        int paramId;
        double value;
    };
    struct FactoryPreset {
        const char* name;
        std::vector<Override> overrides;
    };

    // helpers, all in normalized [0,1] as the plugin's parameters are
    auto wave1 = [](int line, int waveform){
        return Override{lineParam(line, kLineParamWaveformFirst),
                        optionValue(waveform, static_cast<int>(Waveform::kNumWaveforms))};
    };
    auto rate = [](int line, int eg, int step, double seconds){
        return Override{egParam(line, eg, kEgParamRate0 + step), rateForSeconds(seconds)};
    };
    auto level = [](int line, int eg, int step, double value){
        return Override{egParam(line, eg, kEgParamLevel0 + step), value};
    };
    // sustain: 0 = off (the EG runs straight through), n = hold at step n
    auto sustain = [](int line, int eg, int step){
        return Override{egParam(line, eg, kEgParamSustainPoint),
                        optionValue(step, kNumEgSustainPointOptions)};
    };
    // end: the step the EG finishes on, numbered as pd's editor does
    // (2 = the default, i.e. attack then release)
    auto end = [](int line, int eg, int step){
        return Override{egParam(line, eg, kEgParamEndPoint),
                        optionValue(step - 2, kNumEgEndPointOptions)};
    };

    const std::vector<FactoryPreset> factory = {
        {"Init Saw", {}},
        {"Soft Pad", {
            rate(0, kEgDcw, 0, 0.8), level(0, kEgDcw, 0, 0.75), rate(0, kEgDcw, 1, 0.8),
            rate(0, kEgDca, 0, 0.5), rate(0, kEgDca, 1, 1.0),
        }},
        {"E.Piano", {
            wave1(0, 4),  // saw pulse
            rate(0, kEgDcw, 0, 0.0), level(0, kEgDcw, 0, 0.9),
            rate(0, kEgDcw, 1, 0.6), sustain(0, kEgDcw, 0),
            rate(0, kEgDca, 0, 0.0), rate(0, kEgDca, 1, 0.75), sustain(0, kEgDca, 0),
        }},
        {"Brass", {
            rate(0, kEgDcw, 0, 0.1), level(0, kEgDcw, 0, 0.9), rate(0, kEgDcw, 1, 0.2),
            rate(0, kEgDca, 0, 0.025), rate(0, kEgDca, 1, 0.2),
        }},
        {"Reso Sweep", {
            wave1(0, 5),  // resonance I saw tooth
            rate(0, kEgDcw, 0, 1.5), level(0, kEgDcw, 0, 1.0), rate(0, kEgDcw, 1, 0.5),
            rate(0, kEgDca, 0, 0.0), rate(0, kEgDca, 1, 0.2),
        }},
        {"Bell", {
            wave1(0, 3),  // double sine
            {kParamLineSelect, optionValue(2, static_cast<int>(LineSelect::kNumLineSelects))},
            {kParamDetuneFine, optionValue(kDetuneFineRange + 6, 2 * kDetuneFineRange + 1)},
            rate(0, kEgDcw, 0, 0.0), level(0, kEgDcw, 0, 0.85),
            rate(0, kEgDcw, 1, 0.8), sustain(0, kEgDcw, 0),
            rate(0, kEgDca, 0, 0.0), rate(0, kEgDca, 1, 1.0), sustain(0, kEgDca, 0),
        }},
        {"Mono Bass", {
            {kParamMonoPoly, 1.0},
            wave1(0, 2),  // pulse
            // two-stage DCW: bright attack, decay to a darker sustain
            rate(0, kEgDcw, 0, 0.0), level(0, kEgDcw, 0, 0.9),
            rate(0, kEgDcw, 1, 0.2), level(0, kEgDcw, 1, 0.35),
            sustain(0, kEgDcw, 2), end(0, kEgDcw, 3), rate(0, kEgDcw, 2, 0.05),
            rate(0, kEgDca, 0, 0.0), rate(0, kEgDca, 1, 0.05),
        }},
        {"Dual Detune", {
            {kParamLineSelect, optionValue(3, static_cast<int>(LineSelect::kNumLineSelects))},
            {kParamDetuneFine, optionValue(kDetuneFineRange + 12, 2 * kDetuneFineRange + 1)},
            wave1(1, 1),  // line 2 = square
            rate(0, kEgDca, 0, 0.05), rate(0, kEgDca, 1, 0.4),
            rate(1, kEgDca, 0, 0.05), rate(1, kEgDca, 1, 0.4),
        }},
    };

    for(const FactoryPreset& preset : factory){
        for(int paramId = 0; paramId < kNumPdParams; paramId++){
            paramHandles_[paramId]->set(static_cast<float>(defaultParamValue(paramId)));
        }
        for(const Override& item : preset.overrides){
            paramHandles_[item.paramId]->set(static_cast<float>(item.value));
        }
        presets_.add(preset.name);
    }
    presets_.loadCurrent();  // start on slot 0, whatever the last one written was
}

void PdSynthProcessor::prepare(double sampleRate, int maxBlockSize){
    monoScratch_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    for(PdVoice& voice : voices_){
        voice.setSampleRate(sampleRate);
    }
    volumeSmoother_.prepare(sampleRate, 0.005f);
    volumeSmoother_.snap(static_cast<float>(volume_));
}

void PdSynthProcessor::reset(){
    releaseAllVoices();
    heldNotes_.clear();
}

// --- parameter plumbing ------------------------------------------------------

void PdSynthProcessor::applyParameter(int paramId, double value){
    // mirror of PDProcessor::applyParameter (processor.cpp)
    if(paramId == kParamPitchBend){
        pitchBend_ = 2.0 * (value - 0.5);
    }else if(paramId == kParamVolume){
        volume_ = value;
    }else if(paramId == kParamLineSelect){
        lineSelect_ = static_cast<LineSelect>(
            decodeOptionIndex(value, static_cast<int>(LineSelect::kNumLineSelects)));
        for(PdVoice& voice : voices_){
            voice.setLineSelect(lineSelect_);
        }
    }else if(paramId == kParamMonoPoly){
        const bool mono = value >= 0.5;
        if(mono != mono_){
            mono_ = mono;
            releaseAllVoices();
            heldNotes_.clear();
        }
    }else if(paramId == kParamDetuneOctave){
        detuneOctave_ = decodeSignedOption(value, kDetuneOctaveRange);
        updateDetune();
    }else if(paramId == kParamDetuneNote){
        detuneNote_ = decodeSignedOption(value, kDetuneNoteRange);
        updateDetune();
    }else if(paramId == kParamDetuneFine){
        detuneFine_ = decodeSignedOption(value, kDetuneFineRange);
        updateDetune();
    }else if(kParamLine1Begin <= paramId && paramId < kParamCcEditLine){
        const int rel = paramId - kParamLine1Begin;
        for(PdVoice& voice : voices_){
            voice.setLineParam(rel / kNumLineParams, rel % kNumLineParams, value);
        }
    }
    // kParamCcEditLine only steers the CC routing below; kParamMonoTrigger
    // and kParamPolyTrigger exist for the plugin's IMidiMapping (one
    // parameter per CC) and are inert here — the standalone CC map writes
    // kParamMonoPoly directly.
}

int PdSynthProcessor::paramIdForCc(uint8_t cc) const {
    switch(cc){
        case kCcVolume:             return kParamVolume;
        case kCcEditLineController: return kParamCcEditLine;
        case kCcLineSelect:         return kParamLineSelect;
        case kCcDetuneOctave:       return kParamDetuneOctave;
        case kCcDetuneNote:         return kParamDetuneNote;
        case kCcDetuneFine:         return kParamDetuneFine;
        // MIDI Mono/Poly Mode On. The plugin routes these to two dummy
        // parameters because IMidiMapping needs one per CC; standalone the
        // switch itself is the target, and the data byte carries nothing
        // we need (its MIDI meaning is a channel count).
        case kCcMonoModeOn:
        case kCcPolyModeOn:         return kParamMonoPoly;
        default:                    break;
    }

    // The waveform and EG controllers address whichever line CC3 selected,
    // as on hardware where the panel picks the line being edited.
    const int lineBase = (paramHandles_[kParamCcEditLine]->get() >= 0.5f)
                       ? kParamLine2Begin : kParamLine1Begin;
    if(cc == kCcWaveformFirst){
        return lineBase + kLineParamWaveformFirst;
    }
    if(cc == kCcWaveformSecond){
        return lineBase + kLineParamWaveformSecond;
    }
    const int offset = lineParamOffsetForCc(cc);
    return (offset >= 0)? lineBase + offset : -1;
}

int PdSynthProcessor::paramIdForHandle(const Parameter* parameter) const {
    for(int paramId = 0; paramId < kNumPdParams; paramId++){
        if(paramHandles_[paramId] == parameter){
            return paramId;
        }
    }
    return -1;
}

Parameter* PdSynthProcessor::parameterForCc(uint8_t cc){
    const int paramId = paramIdForCc(cc);
    return (paramId >= 0)? paramHandles_[paramId] : nullptr;
}

// Decodes a normalized value the same way pd's Voice does, so a display
// shows what the synth actually got — "Pulse", "Step 2", "180 ms" —
// rather than the 0..1 number a CC happened to produce.
std::string PdSynthProcessor::describeValue(const Parameter& parameter) const {
    const int paramId = paramIdForHandle(&parameter);
    if(paramId < 0){
        return {};
    }
    const double value = parameter.get();
    char text[64];

    switch(paramId){
        case kParamPitchBend:
            std::snprintf(text, sizeof(text), "%+.2f", 2.0 * (value - 0.5));
            return text;
        case kParamVolume:
            std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
            return text;
        case kParamLineSelect: {
            static const char* const names[] = {"Line 1", "Line 2", "1+1'", "1+2'"};
            return names[decodeOptionIndex(
                value, static_cast<int>(LineSelect::kNumLineSelects))];
        }
        case kParamMonoPoly:
            return (value >= 0.5)? "MONO" : "POLY";
        case kParamCcEditLine:
            return (value >= 0.5)? "Line 2" : "Line 1";
        case kParamDetuneOctave:
            std::snprintf(text, sizeof(text), "%+d oct",
                          decodeSignedOption(value, kDetuneOctaveRange));
            return text;
        case kParamDetuneNote:
            std::snprintf(text, sizeof(text), "%+d semitones",
                          decodeSignedOption(value, kDetuneNoteRange));
            return text;
        case kParamDetuneFine: {
            const int steps = decodeSignedOption(value, kDetuneFineRange);
            std::snprintf(text, sizeof(text), "%+d (%+.1f cent)", steps,
                          steps * kDetuneFineStepCents);
            return text;
        }
        default:
            break;
    }

    if(paramId < kParamLine1Begin || paramId >= kParamCcEditLine){
        return {};
    }

    const int offset = (paramId - kParamLine1Begin) % kNumLineParams;
    if(offset == kLineParamWaveformFirst){
        return kWaveformNames[decodeOptionIndex(
            value, static_cast<int>(Waveform::kNumWaveforms))];
    }
    if(offset == kLineParamWaveformSecond){
        const int option = decodeOptionIndex(value, kNumSecondWaveformOptions);
        return (option == 0)? "Off" : kWaveformNames[option - 1];
    }

    const int sub = (offset - kLineParamEgBegin) % kLineParamEgBlockSize;
    if(sub < kNumEgRateParams){
        std::snprintf(text, sizeof(text), "%.3f (%.0f ms)", value,
                      secondsForRate(value) * 1000.0);
        return text;
    }
    if(sub < kEgParamSustainPoint){
        std::snprintf(text, sizeof(text), "%.3f", value);
        return text;
    }
    if(sub == kEgParamSustainPoint){
        const int option = decodeOptionIndex(value, kNumEgSustainPointOptions);
        if(option == 0){
            return "Off";
        }
        std::snprintf(text, sizeof(text), "Step %d", option);
        return text;
    }
    std::snprintf(text, sizeof(text), "Step %d",
                  decodeOptionIndex(value, kNumEgEndPointOptions) + 2);
    return text;
}

void PdSynthProcessor::syncParameters(){
    for(int paramId = 0; paramId < kNumPdParams; paramId++){
        const double value = paramHandles_[paramId]->get();
        if(value != appliedValues_[paramId]){
            appliedValues_[paramId] = value;
            applyParameter(paramId, value);
        }
    }
}

void PdSynthProcessor::setAndApply(int paramId, double value){
    paramHandles_[paramId]->set(static_cast<float>(value));
    appliedValues_[paramId] = paramHandles_[paramId]->get();
    applyParameter(paramId, appliedValues_[paramId]);
}

void PdSynthProcessor::updateDetune(){
    const double cents = 1200.0 * detuneOctave_ + 100.0 * detuneNote_
                       + kDetuneFineStepCents * detuneFine_;
    const double ratio = std::pow(2.0, cents / 1200.0);
    for(PdVoice& voice : voices_){
        voice.setDetuneRatio(ratio);
    }
}

// --- note handling (mirror of PDProcessor's pool + mono logic) ---------------

int PdSynthProcessor::effectiveMaxVoices() const {
    const bool dualLine = lineSelect_ == LineSelect::kLine1Plus1Detuned
                       || lineSelect_ == LineSelect::kLine1Plus2Detuned;
    return dualLine? voiceLimit_ / 2 : voiceLimit_;
}

PdSynthProcessor::PdVoice* PdSynthProcessor::allocateVoice(){
    const int numVoices = effectiveMaxVoices();
    for(int i = 0; i < numVoices; i++){
        if(voices_[static_cast<size_t>(i)].isFree()){
            return &voices_[static_cast<size_t>(i)];
        }
    }
    PdVoice* oldest = &voices_[0];
    for(int i = 1; i < numVoices; i++){
        if(voices_[static_cast<size_t>(i)].age() < oldest->age()){
            oldest = &voices_[static_cast<size_t>(i)];
        }
    }
    return oldest;
}

void PdSynthProcessor::onNoteOn(int channel, int note){
    if(mono_){
        if(heldNotes_.size() < heldNotes_.capacity()){  // stay allocation-free
            heldNotes_.push_back({channel, note});
        }
        voices_[0].noteOn(channel, note, nextVoiceAge_++);  // last-note priority
        return;
    }

    // Retrigger a voice already holding this key instead of stacking a
    // second one (the plugin stacks; a hardware synth should self-heal:
    // if a note-off ever gets lost upstream, the next press+release of
    // the same key fully silences it, and the pool doesn't fill with
    // zombie voices).
    for(PdVoice& voice : voices_){
        if(voice.isHeld(channel, note)){
            voice.noteOn(channel, note, nextVoiceAge_++);
            return;
        }
    }
    allocateVoice()->noteOn(channel, note, nextVoiceAge_++);
}

void PdSynthProcessor::onNoteOff(int channel, int note){
    if(mono_){
        for(int i = static_cast<int>(heldNotes_.size()) - 1; i >= 0; i--){
            if(heldNotes_[static_cast<size_t>(i)].channel == channel
            && heldNotes_[static_cast<size_t>(i)].note == note){
                heldNotes_.erase(heldNotes_.begin() + i);
            }
        }
        if(voices_[0].isHeld(channel, note)){
            if(!heldNotes_.empty()){
                voices_[0].noteOn(heldNotes_.back().channel, heldNotes_.back().note,
                                  nextVoiceAge_++);
            }else{
                voices_[0].noteOff();
            }
        }
        return;
    }

    for(PdVoice& voice : voices_){
        if(voice.isHeld(channel, note)){
            voice.noteOff();
        }
    }
}

void PdSynthProcessor::releaseAllVoices(){
    for(PdVoice& voice : voices_){
        if(voice.isActive()){
            voice.noteOff();
        }
    }
}

// --- rendering ----------------------------------------------------------------

void PdSynthProcessor::handleEvent(const MidiEvent& event){
    switch(event.type){
        case MidiEvent::Type::NoteOn:
            onNoteOn(event.channel, event.data1);
            break;
        case MidiEvent::Type::NoteOff:
            onNoteOff(event.channel, event.data1);
            break;
        case MidiEvent::Type::PitchBend:
            setAndApply(kParamPitchBend,
                        static_cast<double>(event.pitchBend14) / 16383.0);
            break;
        case MidiEvent::Type::ControlChange: {
            if(event.data1 == kCcAllSoundOff || event.data1 == kCcAllNotesOff){
                // pd's Voice exposes no hard kill, so both release
                releaseAllVoices();
                heldNotes_.clear();
                break;
            }
            const int paramId = paramIdForCc(event.data1);
            if(paramId >= 0){
                // CC values arrive unquantized, same as the plugin's
                // host-driven automation (see controller.cpp). The two
                // Mode On messages are the exception: they select a switch
                // position, and their data byte is a channel count.
                const double value = (event.data1 == kCcMonoModeOn)? 1.0
                                   : (event.data1 == kCcPolyModeOn)? 0.0
                                   : static_cast<double>(event.data2) / 127.0;
                setAndApply(paramId, value);
            }
            break;
        }
        case MidiEvent::Type::ProgramChange:
            // Save the edits made to the slot we are leaving, load the new
            // one, then push the whole snapshot into the voices at once.
            if(presets_.select(event.data1)){
                syncParameters();
            }
            break;
    }
}

void PdSynthProcessor::renderSegment(int startFrame, int numFrames){
    for(int i = 0; i < numFrames; i++){
        double mixed = 0.0;
        for(int v = 0; v < voiceLimit_; v++){
            PdVoice& voice = voices_[static_cast<size_t>(v)];
            if(voice.isActive()){
                mixed += voice.generate(pitchBend_);
            }
        }
        monoScratch_[static_cast<size_t>(startFrame + i)] =
            static_cast<float>(kVoiceMixGain * mixed);
    }
}

void PdSynthProcessor::process(AudioBufferView& output, const MidiBuffer& midi){
    const int numFrames = std::min<int>(output.numFrames(),
                                        static_cast<int>(monoScratch_.size()));

    output.clear();
    syncParameters();

    int pos = 0;
    for(const MidiEvent& event : midi){
        const int eventPos = std::clamp(event.sampleOffset, pos, numFrames);
        renderSegment(pos, eventPos - pos);
        handleEvent(event);
        pos = eventPos;
    }
    renderSegment(pos, numFrames - pos);

    // output stage: smoothed volume (pd applies volume_ directly; smoothing
    // it here keeps knob/CC sweeps click-free) + hard clip, all channels
    volumeSmoother_.setTarget(static_cast<float>(volume_));
    for(int i = 0; i < numFrames; i++){
        monoScratch_[static_cast<size_t>(i)] = std::clamp(
            volumeSmoother_.tick() * monoScratch_[static_cast<size_t>(i)], -1.0f, 1.0f);
    }
    for(int ch = 0; ch < output.numChannels(); ch++){
        float* dst = output.channel(ch);
        for(int i = 0; i < numFrames; i++){
            dst[i] = monoScratch_[static_cast<size_t>(i)];
        }
    }
}

}  // namespace rtsynth
