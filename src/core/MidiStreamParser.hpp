#pragma once

#include <cstddef>
#include <cstdint>

#include "MidiBuffer.hpp"

namespace rtsynth {

// Incremental parser for a raw MIDI 1.0 byte stream, as read from an ALSA
// rawmidi device or a UART (DIN MIDI). Unlike the ALSA sequencer path,
// a raw stream is just bytes, so the parser must handle:
//   - running status (a status byte omitted for repeated message types —
//     dense chords from keyboards arrive this way)
//   - system real-time bytes (0xF8..0xFF), which may appear in the middle
//     of any other message and must be transparent
//   - sysex framing (skipped) and system common messages (framed, skipped)
// Complete channel messages are emitted as MidiEvents via the callback;
// unsupported ones (program change, aftertouch, ...) are framed correctly
// and skipped so the stream never desynchronizes.
class MidiStreamParser {
public:
    template <typename Fn>
    void feed(const uint8_t* bytes, size_t size, Fn&& emit){
        for(size_t i = 0; i < size; i++){
            feedByte(bytes[i], emit);
        }
    }

    template <typename Fn>
    void feedByte(uint8_t byte, Fn&& emit){
        if(byte >= 0xF8){
            return;  // system real-time: transparent, never alters state
        }

        if(inSysex_){
            if(byte == 0xF7){
                inSysex_ = false;
                return;
            }
            if((byte & 0x80) == 0){
                return;  // sysex payload
            }
            inSysex_ = false;  // any other status byte terminates sysex
        }

        if(byte & 0x80){  // status byte
            // Any status byte ends whatever came before, including a
            // system-common message whose data bytes never arrived —
            // otherwise a stale skip count would swallow the first data
            // byte of this message and silently corrupt/lose a note.
            pendingSkip_ = 0;

            if(byte == 0xF0){
                inSysex_ = true;
                status_ = 0;
                return;
            }
            if(byte >= 0xF1){  // system common: cancels running status
                status_ = 0;
                // frame away their data bytes (F1/F3: one, F2: two)
                pendingSkip_ = (byte == 0xF2)? 2 : (byte == 0xF1 || byte == 0xF3)? 1 : 0;
                return;
            }
            status_ = byte;
            have_ = 0;
            needed_ = expectedDataBytes(byte);
            return;
        }

        // data byte
        if(pendingSkip_ > 0){
            pendingSkip_--;
            return;
        }
        if(status_ == 0){
            return;  // stray data byte with no status to attach to
        }
        data_[have_++] = byte;
        if(have_ >= needed_){
            const uint8_t message[3] = {
                status_, data_[0], static_cast<uint8_t>((needed_ > 1)? data_[1] : 0)};
            MidiEvent event;
            if(MidiEvent::fromRaw(message, static_cast<size_t>(1 + needed_), event)){
                emit(event);
            }
            have_ = 0;  // keep status_: running status expects fresh data bytes
        }
    }

    void reset(){
        status_ = 0;
        have_ = 0;
        needed_ = 0;
        pendingSkip_ = 0;
        inSysex_ = false;
    }

private:
    static int expectedDataBytes(uint8_t status){
        const uint8_t kind = status & 0xF0;
        return (kind == 0xC0 || kind == 0xD0)? 1 : 2;  // program change / channel pressure
    }

    uint8_t status_ = 0;
    uint8_t data_[2] = {0, 0};
    int have_ = 0;
    int needed_ = 0;
    int pendingSkip_ = 0;
    bool inSysex_ = false;
};

}  // namespace rtsynth
