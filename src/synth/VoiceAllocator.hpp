#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include "Voice.hpp"

namespace rtsynth {

// Owns the voice pool and implements the note-lifecycle policy, keeping
// Voice itself free of any knowledge about its siblings. Everything here
// runs on the audio thread (called from a Processor), so it is plain
// single-threaded code.
//
// Allocation policy for a new note-on:
//   1. the same note is already held        -> retrigger that voice
//   2. a free voice exists                  -> use it
//   3. otherwise steal, preferring the oldest *releasing* voice (already
//      fading, least audible) over the oldest *held* one; the stolen voice
//      fades out briefly before the new note starts (see Voice::stealFor)
//
// Sustain pedal (CC64) semantics: a note-off while the pedal is down only
// *marks* the voice; the actual release happens when the pedal is lifted.
template <size_t NumVoices>
class VoiceAllocator {
public:
    void prepare(double sampleRate){
        for(auto& v : voices_){
            v.prepare(sampleRate);
        }
        orderCounter_ = 0;
        sustainPedal_ = false;
    }

    // runtime polyphony cap (<= 0 restores the full pool); only voices
    // below the cap are ever allocated or rendered
    void setMaxVoices(int maxVoices){
        maxVoices_ = (maxVoices <= 0)
                   ? NumVoices
                   : std::min(static_cast<size_t>(maxVoices), NumVoices);
    }
    size_t maxVoices() const { return maxVoices_; }

    void setEnvelopeParameters(float a, float d, float s, float r){
        for(size_t i = 0; i < maxVoices_; i++){
            voices_[i].setEnvelopeParameters(a, d, s, r);
        }
    }

    void noteOn(uint8_t note, uint8_t velocity){
        Voice* voice = findVoiceForNote(note);   // retrigger same note
        if(voice == nullptr){
            voice = findFreeVoice();
        }
        if(voice != nullptr){
            voice->startNote(note, velocity, ++orderCounter_);
        }else{
            // all voices busy: fade the steal target out first, then sound
            // the new note (click-free)
            findStealTarget()->stealFor(note, velocity, ++orderCounter_);
        }
    }

    void noteOff(uint8_t note){
        Voice* voice = findVoiceForNote(note);
        if(voice == nullptr){
            // the note may still be waiting behind a steal fade
            for(size_t i = 0; i < maxVoices_; i++){
                if(voices_[i].hasPendingNote(note)){
                    voices_[i].cancelPending();
                    return;
                }
            }
            return;
        }
        if(sustainPedal_){
            voice->sustain();
        }else{
            voice->stopNote();
        }
    }

    void setSustainPedal(bool down){
        sustainPedal_ = down;
        if(!down){
            for(size_t i = 0; i < maxVoices_; i++){
                if(voices_[i].isSustained()){
                    voices_[i].stopNote();
                }
            }
        }
    }

    // CC123: release every note as if its key (and the pedal) were lifted.
    void allNotesOff(){
        for(size_t i = 0; i < maxVoices_; i++){
            voices_[i].cancelPending();
            if(voices_[i].isHeld() || voices_[i].isSustained()){
                voices_[i].stopNote();
            }
        }
    }

    // CC120: hard-stop everything immediately (panic button; may click).
    void allSoundOff(){
        for(auto& v : voices_){
            v.kill();
        }
    }

    void render(float* dst, int numFrames, double pitchBendRatio){
        for(size_t i = 0; i < maxVoices_; i++){
            voices_[i].render(dst, numFrames, pitchBendRatio);
        }
    }

    // diagnostic gauge; may be read from another thread (approximate)
    int activeCount() const {
        int count = 0;
        for(size_t i = 0; i < maxVoices_; i++){
            if(voices_[i].isActive()){
                count++;
            }
        }
        return count;
    }

private:
    // newest held (non-releasing) voice playing `note`
    Voice* findVoiceForNote(uint8_t note){
        Voice* found = nullptr;
        uint64_t newest = 0;
        for(size_t i = 0; i < maxVoices_; i++){
            Voice& v = voices_[i];
            if(v.isHeld() && !v.isSustained() && v.note() == note && v.order() > newest){
                newest = v.order();
                found = &v;
            }
        }
        return found;
    }

    Voice* findFreeVoice(){
        for(size_t i = 0; i < maxVoices_; i++){
            if(!voices_[i].isActive()){
                return &voices_[i];
            }
        }
        return nullptr;
    }

    Voice* findStealTarget(){
        Voice* oldestReleasing = nullptr;
        Voice* oldestHeld = nullptr;
        uint64_t oldestReleasingOrder = UINT64_MAX;
        uint64_t oldestHeldOrder = UINT64_MAX;
        for(size_t i = 0; i < maxVoices_; i++){
            Voice& v = voices_[i];
            if(v.isReleasing() && v.order() < oldestReleasingOrder){
                oldestReleasingOrder = v.order();
                oldestReleasing = &v;
            }else if(v.isHeld() && v.order() < oldestHeldOrder){
                oldestHeldOrder = v.order();
                oldestHeld = &v;
            }
        }
        return (oldestReleasing != nullptr)? oldestReleasing : oldestHeld;
    }

    std::array<Voice, NumVoices> voices_;
    size_t maxVoices_ = NumVoices;
    uint64_t orderCounter_ = 0;
    bool sustainPedal_ = false;
};

}  // namespace rtsynth
