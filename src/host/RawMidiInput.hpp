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
//
// Hot-plug works the same way as in RtMidiInput: open() succeeds with
// nothing attached, a reader thread that sees its device disappear marks
// the slot dead, and rescan() (main thread) reaps it and opens whatever
// has appeared. Device slots are append-only and published atomically,
// because the audio thread walks them in pop() while this happens.
class RawMidiInput : public MidiInput {
public:
    static constexpr size_t kMaxDevices = 16;

    // (device id, human-readable name), e.g. ("hw:1,0,0", "KL Essential 49")
    static std::vector<std::pair<std::string, std::string>> listInputs();

    // deviceIds e.g. {"hw:1,0,0"}, or {"all"} to follow every device the
    // kernel exposes (including ones plugged in later). Returns false when
    // nothing was opened, which is not fatal: rescan() keeps trying.
    bool open(const std::vector<std::string>& deviceIds);
    void close() override;

    bool rescan() override { return scan(false); }
    bool connected() const override { return connectedDevices() > 0; }

    std::string description() const override;

    bool pop(MidiEvent& out) override {
        const size_t count = deviceCount_.load(std::memory_order_acquire);
        for(size_t i = 0; i < count; i++){
            if(devices_[i]->queue.pop(out)){
                return true;
            }
        }
        return false;
    }

    uint64_t receivedCount() const override { return received_.load(std::memory_order_relaxed); }
    uint64_t droppedCount() const override { return dropped_.load(std::memory_order_relaxed); }
    uint64_t undecodedCount() const override { return 0; }  // parser frames everything
    uint64_t readErrorCount() const override { return readErrors_.load(std::memory_order_relaxed); }

    void setMonitorEnabled(bool enabled) override {
        monitor_.store(enabled, std::memory_order_relaxed);
    }

    void setRawDumpEnabled(bool enabled) override {
        rawDump_.store(enabled, std::memory_order_relaxed);
    }

    void drainRawDump(const RawByteFn& fn) override {
        const size_t count = deviceCount_.load(std::memory_order_acquire);
        for(size_t i = 0; i < count; i++){
            RawMidiByte entry;
            while(devices_[i]->rawQueue.pop(entry)){
                fn(entry.value, entry.startsGroup);
            }
        }
    }

    void drainMonitor(const MonitorFn& fn) override {
        const size_t count = deviceCount_.load(std::memory_order_acquire);
        for(size_t i = 0; i < count; i++){
            MidiEvent event;
            while(devices_[i]->monitorQueue.pop(event)){
                fn(devices_[i]->name, event);
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
        SpscRingBuffer<RawMidiByte, 8192> rawQueue;       // consumer: main thread
        std::thread thread;
        // set by the reader thread when the device stops existing (unplug),
        // read by the main thread in scan() to reap the slot
        std::atomic<bool> gone{false};
        bool connected = false;  // main thread only
    };

    void readerThread(Device& device);
    bool scan(bool initial);
    bool connectDevice(const std::string& id, const std::string& name, bool initial);
    void reapDevice(Device& device);
    size_t connectedDevices() const;

    // append-only slots; the audio thread reads devices_[0, deviceCount_)
    std::unique_ptr<Device> devices_[kMaxDevices];
    std::atomic<size_t> deviceCount_{0};
    std::vector<std::string> requested_;  // ids from --midi-raw
    bool wantAll_ = false;                // --midi-raw all: follow everything
    bool slotsExhausted_ = false;
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> readErrors_{0};
    std::atomic<bool> monitor_{false};
    std::atomic<bool> rawDump_{false};
    std::atomic<bool> running_{false};
};

}  // namespace rtsynth
