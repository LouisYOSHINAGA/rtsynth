#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <RtMidi.h>
#include "../core/MidiBuffer.hpp"
#include "../core/SpscRingBuffer.hpp"

namespace rtsynth {

// Host-side MIDI input: receives raw messages on RtMidi's callback thread,
// decodes them to MidiEvent and hands them to the audio thread through a
// lock-free queue. Nothing here blocks or allocates after open().
class RtMidiInput {
public:
    std::vector<std::string> listPorts();

    // portIndex < 0 selects the first port that is not "Midi Through"
    bool open(int portIndex);
    void close();

    const std::string& openedPortName() const { return portName_; }

    // consumer side (audio thread)
    bool pop(MidiEvent& out){ return queue_.pop(out); }

    uint64_t receivedCount() const { return received_.load(std::memory_order_relaxed); }
    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static void rtCallback(double timestamp, std::vector<uint8_t>* message, void* userData);

    // creating RtMidiIn can throw (e.g. no ALSA sequencer on headless
    // systems), so construct it lazily and report failure as "no ports"
    bool ensureCreated();

    std::unique_ptr<RtMidiIn> midi_;
    std::string portName_;
    SpscRingBuffer<MidiEvent, 1024> queue_;
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace rtsynth
