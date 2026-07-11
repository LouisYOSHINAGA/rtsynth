#include <cmath>
#include <algorithm>
#include "SineSynthProcessor.hpp"

namespace rtsynth {

namespace {
constexpr uint8_t kCcVolume = 7;
constexpr uint8_t kCcSustainPedal = 64;
constexpr uint8_t kCcAllSoundOff = 120;
constexpr uint8_t kCcAllNotesOff = 123;
}

SineSynthProcessor::SineSynthProcessor(){
    gain_ = parameters_.add("gain", "Master Gain", 0.0f, 1.0f, 0.2f);
    attack_ = parameters_.add("attack", "Attack", 0.001f, 5.0f, 0.005f, "s");
    decay_ = parameters_.add("decay", "Decay", 0.001f, 5.0f, 0.1f, "s");
    sustain_ = parameters_.add("sustain", "Sustain", 0.0f, 1.0f, 0.8f);
    release_ = parameters_.add("release", "Release", 0.001f, 5.0f, 0.05f, "s");
    bendRange_ = parameters_.add("bend_range", "Pitch Bend Range", 0.0f, 24.0f, 2.0f, "semitones");
}

void SineSynthProcessor::prepare(double sampleRate, int maxBlockSize){
    sampleRate_ = sampleRate;
    monoScratch_.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    voices_.prepare(sampleRate);
    pitchBendRatio_ = 1.0;
}

void SineSynthProcessor::reset(){
    voices_.allSoundOff();
    pitchBendRatio_ = 1.0;
}

void SineSynthProcessor::process(AudioBufferView& output, const MidiBuffer& midi){
    const int numFrames = std::min<int>(output.numFrames(),
                                        static_cast<int>(monoScratch_.size()));

    // if the driver ever delivers a larger block than prepare() sized us
    // for, the tail beyond numFrames must still be valid (silent) output
    output.clear();
    std::fill(monoScratch_.begin(), monoScratch_.begin() + numFrames, 0.0f);
    voices_.setEnvelopeParameters(attack_->get(), decay_->get(),
                                  sustain_->get(), release_->get());

    // Render the audio in segments split at each MIDI event's offset, so
    // an event exactly affects the samples after its position ("sample-
    // accurate" processing, the same scheme VST3/JUCE plugins use):
    //
    //   |----seg----|ev|--seg--|ev|-------seg-------|   one block
    int pos = 0;
    for(const MidiEvent& event : midi){
        const int eventPos = std::clamp(event.sampleOffset, pos, numFrames);
        renderSegment(pos, eventPos - pos);
        handleEvent(event);
        pos = eventPos;
    }
    renderSegment(pos, numFrames - pos);

    // output stage: gain + hard clip, copied to every output channel
    const float gain = gain_->get();
    for(int i = 0; i < numFrames; i++){
        monoScratch_[static_cast<size_t>(i)] =
            std::clamp(gain * monoScratch_[static_cast<size_t>(i)], -1.0f, 1.0f);
    }
    for(int ch = 0; ch < output.numChannels(); ch++){
        float* dst = output.channel(ch);
        for(int i = 0; i < numFrames; i++){
            dst[i] = monoScratch_[static_cast<size_t>(i)];
        }
    }
}

void SineSynthProcessor::renderSegment(int startFrame, int numFrames){
    if(numFrames <= 0){
        return;
    }
    voices_.render(monoScratch_.data() + startFrame, numFrames, pitchBendRatio_);
}

void SineSynthProcessor::handleEvent(const MidiEvent& event){
    switch(event.type){
        case MidiEvent::Type::NoteOn:
            voices_.noteOn(event.data1, event.data2);
            break;
        case MidiEvent::Type::NoteOff:
            voices_.noteOff(event.data1);
            break;
        case MidiEvent::Type::PitchBend: {
            const double bend = (static_cast<double>(event.pitchBend14) - 8192.0) / 8192.0;
            pitchBendRatio_ = std::pow(2.0, bend * bendRange_->get() / 12.0);
            break;
        }
        case MidiEvent::Type::ControlChange:
            switch(event.data1){
                case kCcVolume:
                    gain_->setNormalized(static_cast<float>(event.data2) / 127.0f);
                    break;
                case kCcSustainPedal:
                    voices_.setSustainPedal(event.data2 >= 64);
                    break;
                case kCcAllSoundOff:
                    voices_.allSoundOff();
                    break;
                case kCcAllNotesOff:
                    voices_.allNotesOff();
                    break;
                default:
                    break;
            }
            break;
    }
}

}  // namespace rtsynth
