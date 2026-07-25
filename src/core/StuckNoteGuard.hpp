#pragma once

#include <cstdint>

#include "MidiBuffer.hpp"

namespace rtsynth {

// Recovery for MIDI sources that lose note-offs.
//
// Some controllers (and marginal USB links) occasionally fail to send a
// note-off — typically one note of a chord released simultaneously, which
// a byte-level dump shows as three note-ons answered by only two
// note-offs on the wire. The synth then holds that voice forever: the note
// drones until the player happens to press the same key again. Nothing
// downstream can tell "key still held" from "note-off was lost", so the
// host keeps its own record of which notes are down and for how long, and
// injects a synthetic note-off once a note outlives the timeout.
//
// This is a last-resort net, not an envelope: the timeout must stay long
// enough that deliberately sustained notes (pads, drones) are never cut
// short, so it is configurable and can be switched off entirely. The real
// fix always lives in the controller.
//
// Time is counted in frames handed to advance(), so the audio thread needs
// no clock syscall, and the guard allocates nothing.
class StuckNoteGuard {
public:
    static constexpr size_t kMaxHeldNotes = 128;

    // timeoutSeconds <= 0 disables the guard entirely
    void prepare(double sampleRate, float timeoutSeconds){
        timeoutFrames_ = (timeoutSeconds > 0.0f)
            ? static_cast<uint64_t>(static_cast<double>(timeoutSeconds) * sampleRate)
            : 0;
        reset();
    }

    void reset(){
        heldCount_ = 0;
        now_ = 0;
    }

    bool enabled() const { return timeoutFrames_ > 0; }
    uint64_t recoveredCount() const { return recovered_; }
    size_t heldCount() const { return heldCount_; }

    // Track what the instrument is being told. Call for every event that
    // reaches the processor, in order.
    void observe(const MidiEvent& event){
        if(!enabled()){
            return;
        }
        switch(event.type){
            case MidiEvent::Type::NoteOn:
                markHeld(event.channel, event.data1);
                break;
            case MidiEvent::Type::NoteOff:
                clearHeld(event.channel, event.data1);
                break;
            case MidiEvent::Type::ControlChange:
                // 120 = all sound off, 123 = all notes off: the instrument
                // silences everything, so our record must follow
                if(event.data1 == 120 || event.data1 == 123){
                    heldCount_ = 0;
                }
                break;
            case MidiEvent::Type::PitchBend:
                break;
        }
    }

    void advance(int frames){
        now_ += static_cast<uint64_t>(frames);
    }

    // Emits a note-off for every note held longer than the timeout.
    // `emit` returns false when the caller could not accept the event, in
    // which case the note stays tracked and is retried on the next block.
    template <typename Fn>
    void collectExpired(Fn&& emit){
        if(!enabled()){
            return;
        }
        for(size_t i = 0; i < heldCount_; ){
            if(now_ - held_[i].since <= timeoutFrames_){
                i++;
                continue;
            }
            if(!emit(MidiEvent::noteOff(held_[i].channel, held_[i].note))){
                return;  // buffer full: try again next block
            }
            recovered_++;
            removeAt(i);
        }
    }

private:
    struct HeldNote {
        uint8_t channel;
        uint8_t note;
        uint64_t since;
    };

    void markHeld(uint8_t channel, uint8_t note){
        for(size_t i = 0; i < heldCount_; i++){
            if(held_[i].channel == channel && held_[i].note == note){
                held_[i].since = now_;  // retrigger restarts the clock
                return;
            }
        }
        if(heldCount_ < kMaxHeldNotes){
            held_[heldCount_++] = {channel, note, now_};
        }
    }

    void clearHeld(uint8_t channel, uint8_t note){
        for(size_t i = 0; i < heldCount_; i++){
            if(held_[i].channel == channel && held_[i].note == note){
                removeAt(i);
                return;
            }
        }
    }

    void removeAt(size_t index){
        held_[index] = held_[--heldCount_];  // order is irrelevant here
    }

    HeldNote held_[kMaxHeldNotes]{};
    size_t heldCount_ = 0;
    uint64_t now_ = 0;
    uint64_t timeoutFrames_ = 0;
    uint64_t recovered_ = 0;
};

}  // namespace rtsynth
