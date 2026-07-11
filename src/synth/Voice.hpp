#pragma once

#include <cmath>
#include <cstdint>
#include "../dsp/SineOscillator.hpp"
#include "../dsp/AdsrEnvelope.hpp"

namespace rtsynth {

// 12-TET MIDI note number -> frequency in Hz (A4 = note 69 = 440 Hz)
inline double noteToFrequency(int note){
    constexpr double kA4Freq = 440.0;
    constexpr int kA4Note = 69;
    return kA4Freq * std::pow(2.0, (note - kA4Note) / 12.0);
}

// One polyphonic voice: oscillator + amplitude envelope + note state.
// A voice moves through these states (managed by VoiceAllocator):
//
//   free --startNote()--> held --stopNote()--> releasing --(env ends)--> free
//                          |                       ^
//                          +------stealFor()-------+--> fade, then the
//                                                       pending note starts
//
// "Stealing" never cuts a sounding voice instantly: stealFor() remembers
// the new note, fades the old one out in a few milliseconds, and render()
// starts the pending note once the fade has reached silence.
class Voice {
public:
    void prepare(double sampleRate){
        osc_.setSampleRate(sampleRate);
        env_.setSampleRate(sampleRate);
        env_.reset();
        order_ = 0;
        sustained_ = false;
        hasPending_ = false;
    }

    void setEnvelopeParameters(float a, float d, float s, float r){
        env_.setParameters(a, d, s, r);
    }

    void startNote(uint8_t note, uint8_t velocity, uint64_t order){
        note_ = note;
        baseFrequency_ = noteToFrequency(note);
        // velocity -> gain; squared curve feels more natural than linear
        const float v = static_cast<float>(velocity) / 127.0f;
        velocityGain_ = v * v;
        order_ = order;
        sustained_ = false;
        if(!env_.isActive()){
            // fresh voice: start from phase 0. When retriggering a voice
            // that is still sounding, keep the phase (and let the envelope
            // attack from its current level) so there is no discontinuity.
            osc_.resetPhase();
        }
        env_.noteOn();
    }

    void stopNote(){
        sustained_ = false;
        env_.noteOff();
    }

    // note-off arrived while the sustain pedal is held
    void sustain(){ sustained_ = true; }
    bool isSustained() const { return sustained_; }

    // Steal this voice for a new note: fade out quickly (click-free) and
    // start the new note once the fade reaches silence (see render()).
    void stealFor(uint8_t note, uint8_t velocity, uint64_t order){
        pendingNote_ = note;
        pendingVelocity_ = velocity;
        hasPending_ = true;
        order_ = order;  // count as newest so it isn't stolen again right away
        sustained_ = false;
        env_.fastRelease();
    }

    bool hasPendingNote(uint8_t note) const {
        return hasPending_ && pendingNote_ == note;
    }
    void cancelPending(){ hasPending_ = false; }

    void kill(){
        env_.reset();
        order_ = 0;
        sustained_ = false;
        hasPending_ = false;
    }

    bool isActive() const { return env_.isActive(); }
    bool isReleasing() const { return env_.isReleasing(); }
    bool isHeld() const { return env_.isActive() && !env_.isReleasing(); }
    uint8_t note() const { return note_; }
    uint64_t order() const { return order_; }
    float envelopeLevel() const { return env_.level(); }

    // Accumulate (+=) `numFrames` mono samples into `dst`; the caller mixes
    // voices by passing the same buffer to each. `pitchBendRatio` is a
    // frequency multiplier (1.0 = no bend), applied per render call.
    void render(float* dst, int numFrames, double pitchBendRatio){
        if(!env_.isActive()){
            if(hasPending_){  // steal fade finished: start the queued note
                hasPending_ = false;
                startNote(pendingNote_, pendingVelocity_, order_);
            }else{
                return;
            }
        }
        osc_.setFrequency(baseFrequency_ * pitchBendRatio);
        for(int i = 0; i < numFrames; i++){
            dst[i] += velocityGain_ * env_.tick() * osc_.tick();
            if(!env_.isActive()){
                if(!hasPending_){
                    order_ = 0;
                }
                break;  // a pending stolen note starts on the next render call
            }
        }
    }

private:
    SineOscillator osc_;
    AdsrEnvelope env_;
    uint8_t note_ = 0;
    double baseFrequency_ = 0.0;
    float velocityGain_ = 0.0f;
    uint64_t order_ = 0;   // monotonically increasing note-on counter (0 = never used)
    bool sustained_ = false;
    uint8_t pendingNote_ = 0;
    uint8_t pendingVelocity_ = 0;
    bool hasPending_ = false;
};

}  // namespace rtsynth
