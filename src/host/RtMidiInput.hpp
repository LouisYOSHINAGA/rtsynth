#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <RtMidi.h>

#include "../core/SpscRingBuffer.hpp"
#include "MidiInput.hpp"

namespace rtsynth {

// ALSA-sequencer MIDI input (via RtMidi). By default it connects to
// *every* input port (except the ALSA "Midi Through" loopback), so notes
// from a keyboard and CC from a separate controller can arrive in
// parallel; pass an explicit port index to restrict input to one device.
//
// Threading: RtMidi runs one callback thread per opened port, and the
// SPSC ring buffer allows only a single producer — so each port gets its
// own queue (that port's callback thread is the sole producer, the audio
// thread the sole consumer). pop() drains all of them. Nothing here
// blocks or allocates after open().
class RtMidiInput : public MidiInput {
public:
    std::vector<std::string> listPorts();

    // portIndex >= 0 opens exactly that port; portIndex < 0 opens all
    // ports except "Midi Through". Returns false when nothing was opened.
    bool open(int portIndex);
    void close() override;

    size_t numOpenPorts() const { return ports_.size(); }
    std::string description() const override;

    bool pop(MidiEvent& out) override {
        for(auto& port : ports_){
            if(port->queue.pop(out)){
                return true;
            }
        }
        return false;
    }

    uint64_t receivedCount() const override { return received_.load(std::memory_order_relaxed); }
    uint64_t droppedCount() const override { return dropped_.load(std::memory_order_relaxed); }
    uint64_t undecodedCount() const override { return undecoded_.load(std::memory_order_relaxed); }

    void setMonitorEnabled(bool enabled) override {
        monitor_.store(enabled, std::memory_order_relaxed);
    }

    void setRawDumpEnabled(bool enabled) override {
        rawDump_.store(enabled, std::memory_order_relaxed);
    }

    // The sequencer hands us whole decoded messages, so each message —
    // not each device read — opens a group here.
    void drainRawDump(const RawByteFn& fn) override {
        for(auto& port : ports_){
            RawMidiByte entry;
            while(port->rawQueue.pop(entry)){
                fn(entry.value, entry.startsGroup);
            }
        }
    }

    void drainMonitor(const MonitorFn& fn) override {
        for(auto& port : ports_){
            MidiEvent event;
            while(port->monitorQueue.pop(event)){
                fn(port->name, event);
            }
        }
    }

private:
    struct Port {
        RtMidiInput* owner = nullptr;
        std::unique_ptr<RtMidiIn> midi;
        // generous size: this queue must absorb the backlog that builds up
        // whenever the audio callback stalls (xrun recovery, CPU spikes) —
        // losing a note-off here means a note hangs forever
        SpscRingBuffer<MidiEvent, 4096> queue;
        SpscRingBuffer<MidiEvent, 256> monitorQueue;  // consumer: main thread
        SpscRingBuffer<RawMidiByte, 8192> rawQueue;   // consumer: main thread
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
    std::atomic<uint64_t> undecoded_{0};
    std::atomic<bool> monitor_{false};
    std::atomic<bool> rawDump_{false};
};

}  // namespace rtsynth
