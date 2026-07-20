#include <algorithm>
#include <cmath>

#include "PdSynthProcessor.hpp"

namespace rtsynth {

using namespace Steinberg::Vst;

namespace {

constexpr uint8_t kCcVolume = 7;
constexpr uint8_t kCcAllSoundOff = 120;
constexpr uint8_t kCcAllNotesOff = 123;

// normalized encodings of the discrete init-patch choices (see const.h):
// sustain point option 1 of 8 -> the EG sustains at step 1
constexpr double kSustainAtStep1 = 1.0 / (kNumEgSustainPointOptions - 1);

}  // namespace

PdSynthProcessor::PdSynthProcessor(){
    heldNotes_.reserve(256);  // mono mode bookkeeping; never grows in process()
    registerParameters();
    for(int paramId = 0; paramId < kNumPdParams; paramId++){
        appliedValues_[paramId] = paramHandles_[paramId]->get();
        applyParameter(paramId, appliedValues_[paramId]);
    }
}

std::string PdSynthProcessor::lineParamId(int line, int offset){
    std::string id = "line" + std::to_string(line + 1) + "_";
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

void PdSynthProcessor::registerParameters(){
    paramHandles_[kParamPitchBend] = parameters_.add(
        "pitch_bend", "Pitch Bend", 0.0f, 1.0f,
        static_cast<float>(defaultParamValue(kParamPitchBend)));
    paramHandles_[kParamVolume] = parameters_.add(
        "volume", "Volume", 0.0f, 1.0f,
        static_cast<float>(defaultParamValue(kParamVolume)));
    paramHandles_[kParamLineSelect] = parameters_.add(
        "line_select", "Line Select (1 / 2 / 1+1' / 1+2')", 0.0f, 1.0f, 0.0f);
    paramHandles_[kParamMonoPoly] = parameters_.add(
        "mono", "Mono (SOLO)", 0.0f, 1.0f, 0.0f);
    paramHandles_[kParamDetuneOctave] = parameters_.add(
        "detune_octave", "Detune Octave", 0.0f, 1.0f, 0.5f);
    paramHandles_[kParamDetuneNote] = parameters_.add(
        "detune_note", "Detune Note", 0.0f, 1.0f, 0.5f);
    paramHandles_[kParamDetuneFine] = parameters_.add(
        "detune_fine", "Detune Fine", 0.0f, 1.0f, 0.5f);

    for(int line = 0; line < 2; line++){
        for(int offset = 0; offset < kNumLineParams; offset++){
            const int paramId = kParamLine1Begin + line * kNumLineParams + offset;
            const std::string id = lineParamId(line, offset);
            paramHandles_[paramId] = parameters_.add(
                id, id, 0.0f, 1.0f, static_cast<float>(defaultParamValue(paramId)));
        }
    }

    // VST-only routing switch; registered to keep ids aligned with pd
    paramHandles_[kParamCcEditLine] = parameters_.add(
        "cc_edit_line", "CC Edit Line (unused standalone)", 0.0f, 1.0f, 0.0f);
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
    // kParamCcEditLine has no effect standalone
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
    return dualLine? kMaxVoices / 2 : kMaxVoices;
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
        case MidiEvent::Type::ControlChange:
            switch(event.data1){
                case kCcVolume:
                    setAndApply(kParamVolume, static_cast<double>(event.data2) / 127.0);
                    break;
                case kCcAllSoundOff:
                case kCcAllNotesOff:
                    // pd's Voice exposes no hard kill, so both release
                    releaseAllVoices();
                    heldNotes_.clear();
                    break;
                default:
                    break;
            }
            break;
    }
}

void PdSynthProcessor::renderSegment(int startFrame, int numFrames){
    for(int i = 0; i < numFrames; i++){
        double mixed = 0.0;
        for(PdVoice& voice : voices_){
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
