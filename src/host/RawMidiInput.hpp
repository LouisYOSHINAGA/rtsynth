#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>

#include "../core/MidiStreamParser.hpp"
#include "../core/SpscRingBuffer.hpp"
#include "MidiInput.hpp"

namespace rtsynth {

// MIDI input read straight from the kernel rawmidi device (hw:card,dev,sub)
// — the shortest possible path from a physical keyboard to the synth:
//
//   sequencer route:  USB driver -> rawmidi -> seq bridge -> seq FIFO
//                     -> RtMidi thread -> callback        (RtMidiInput)
//   raw route:        USB driver -> rawmidi -> read()     (this class)
//
// Every hop the raw route skips is one that can reorder, delay or drop
// events; use --midi-raw when dense chords lose note-on/offs through the
// sequencer. The trade-off: only real hardware devices are visible (no
// virtual/software ports), and the device is read exclusively.
//
// One reader thread per device: poll() + nonblocking snd_rawmidi_read()
// into a MidiStreamParser (running status, interleaved real-time bytes),
// then into the same per-device SPSC queue scheme as RtMidiInput.
class RawMidiInput : public MidiInput {
public:
    // (device id, human-readable name), e.g. ("hw:1,0,0", "KL Essential 49")
    static std::vector<std::pair<std::string, std::string>> listInputs();

    // deviceIds e.g. {"hw:1,0,0"}; returns false when nothing was opened
    bool open(const std::vector<std::string>& deviceIds);
    void close() override;

    std::string description() const override;

    bool pop(MidiEvent& out) override {
        for(auto& device : devices_){
            if(device->queue.pop(out)){
                return true;
            }
        }
        return false;
    }

    uint64_t receivedCount() const override { return received_.load(std::memory_order_relaxed); }
    uint64_t droppedCount() const override { return dropped_.load(std::memory_order_relaxed); }
    uint64_t undecodedCount() const override { return 0; }  // parser frames everything

    void setMonitorEnabled(bool enabled) override {
        monitor_.store(enabled, std::memory_order_relaxed);
    }

    void drainMonitor(const MonitorFn& fn) override {
        for(auto& device : devices_){
            MidiEvent event;
            while(device->monitorQueue.pop(event)){
                fn(device->name, event);
            }
        }
    }

private:
    struct Device {
        RawMidiInput* owner = nullptr;
        snd_rawmidi_t* in = nullptr;
        std::string id;
        std::string name;
        MidiStreamParser parser;               // reader thread only
        SpscRingBuffer<MidiEvent, 4096> queue;
        SpscRingBuffer<MidiEvent, 256> monitorQueue;  // consumer: main thread
        std::thread thread;
    };

    void readerThread(Device& device);

    std::vector<std::unique_ptr<Device>> devices_;
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> monitor_{false};
    std::atomic<bool> running_{false};
};

}  // namespace rtsynth
