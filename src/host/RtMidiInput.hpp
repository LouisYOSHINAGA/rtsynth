#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <RtMidi.h>
#include "../core/MidiBuffer.hpp"
#include "../core/SpscRingBuffer.hpp"

namespace rtsynth {

// Host-side MIDI input. By default it connects to *every* input port
// (except the ALSA "Midi Through" loopback), so notes from a keyboard and
// CC from a separate controller can arrive in parallel; pass an explicit
// port index to restrict input to a single device.
//
// Threading: RtMidi runs one callback thread per opened port, and the
// SPSC ring buffer allows only a single producer — so each port gets its
// own queue (that port's callback thread is the sole producer, the audio
// thread the sole consumer). pop() drains all of them. Nothing here
// blocks or allocates after open().
class RtMidiInput {
public:
    std::vector<std::string> listPorts();

    // portIndex >= 0 opens exactly that port; portIndex < 0 opens all
    // ports except "Midi Through". Returns false when nothing was opened.
    bool open(int portIndex);
    void close();

    size_t numOpenPorts() const { return ports_.size(); }
    // ", "-joined port names, for startup logging
    std::string openedPortNames() const;

    // consumer side (audio thread): drains the per-port queues in turn
    bool pop(MidiEvent& out){
        for(auto& port : ports_){
            if(port->queue.pop(out)){
                return true;
            }
        }
        return false;
    }

    uint64_t receivedCount() const { return received_.load(std::memory_order_relaxed); }
    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Port {
        RtMidiInput* owner = nullptr;
        std::unique_ptr<RtMidiIn> midi;
        SpscRingBuffer<MidiEvent, 1024> queue;
        std::string name;
    };

    static void rtCallback(double timestamp, std::vector<uint8_t>* message, void* userData);

    // creating RtMidiIn can throw (e.g. no ALSA sequencer on headless
    // systems), so construct instances lazily and report failure cleanly
    bool ensureProbe();
    bool openOne(unsigned int index, const std::string& name);

    // instance used only for enumerating ports; each opened port gets its
    // own RtMidiIn (RtMidi binds one connection per instance)
    std::unique_ptr<RtMidiIn> probe_;
    std::vector<std::unique_ptr<Port>> ports_;
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace rtsynth
