#pragma once

#include <cstdint>
#include <cstddef>

namespace rtsynth {

// A decoded MIDI event with a sample-accurate position inside the current
// audio block, similar to VST3's Event / JUCE's MidiMessage + sample number.
struct MidiEvent {
    enum class Type : uint8_t {
        NoteOn,
        NoteOff,
        ControlChange,
        PitchBend,
    };

    Type type = Type::NoteOn;
    int32_t sampleOffset = 0;   // frame index within the block ([0, blockSize))
    uint8_t channel = 0;        // 0-15
    uint8_t data1 = 0;          // note number / controller number
    uint8_t data2 = 0;          // velocity / controller value
    uint16_t pitchBend14 = 8192; // 14bit pitch bend value (center = 8192)

    static MidiEvent noteOn(uint8_t ch, uint8_t note, uint8_t velocity, int32_t offset = 0){
        return {Type::NoteOn, offset, ch, note, velocity, 8192};
    }
    static MidiEvent noteOff(uint8_t ch, uint8_t note, int32_t offset = 0){
        return {Type::NoteOff, offset, ch, note, 0, 8192};
    }
    static MidiEvent controlChange(uint8_t ch, uint8_t cc, uint8_t value, int32_t offset = 0){
        return {Type::ControlChange, offset, ch, cc, value, 8192};
    }
    static MidiEvent pitchBend(uint8_t ch, uint16_t value14, int32_t offset = 0){
        return {Type::PitchBend, offset, ch, 0, 0, value14};
    }

    // Decode a raw MIDI message (status + data bytes). Returns false for
    // messages this synth does not handle (sysex, aftertouch, ...).
    static bool fromRaw(const uint8_t* bytes, size_t size, MidiEvent& out, int32_t offset = 0){
        if(size < 2){
            return false;
        }
        const uint8_t status = bytes[0] & 0xF0;
        const uint8_t channel = bytes[0] & 0x0F;
        switch(status){
            case 0x90:
                if(size < 3) return false;
                if(bytes[2] == 0){  // running-status note off
                    out = noteOff(channel, bytes[1], offset);
                }else{
                    out = noteOn(channel, bytes[1], bytes[2], offset);
                }
                return true;
            case 0x80:
                out = noteOff(channel, bytes[1], offset);
                return true;
            case 0xB0:
                if(size < 3) return false;
                out = controlChange(channel, bytes[1], bytes[2], offset);
                return true;
            case 0xE0:
                if(size < 3) return false;
                out = pitchBend(channel, static_cast<uint16_t>(bytes[1] | (bytes[2] << 7)), offset);
                return true;
            default:
                return false;
        }
    }
};

// Fixed-capacity event list for one audio block (no allocation, RT-safe),
// the equivalent of VST3's IEventList / JUCE's MidiBuffer. The host fills
// it before each Processor::process() call and clears it afterwards.
// Events must be added in non-decreasing sampleOffset order — process()
// implementations rely on that to split the block at event boundaries.
// add() returns false when full; the host counts such drops.
template <size_t Capacity>
class BasicMidiBuffer {
public:
    bool add(const MidiEvent& event){
        if(size_ >= Capacity){
            return false;
        }
        events_[size_++] = event;
        return true;
    }

    void clear(){ size_ = 0; }

    size_t size() const { return size_; }
    const MidiEvent* begin() const { return events_; }
    const MidiEvent* end() const { return events_ + size_; }

private:
    MidiEvent events_[Capacity];
    size_t size_ = 0;
};

using MidiBuffer = BasicMidiBuffer<256>;

}  // namespace rtsynth
