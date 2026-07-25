#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../core/MidiBuffer.hpp"

namespace rtsynth {

// Runtime surface shared by the MIDI input backends:
//
//   RtMidiInput   ALSA sequencer via RtMidi — flexible (virtual ports,
//                 software sources), but the events travel USB driver ->
//                 kernel rawmidi -> seq bridge -> seq FIFO -> RtMidi
//                 thread before reaching us
//   RawMidiInput  kernel rawmidi device read directly — the shortest
//                 possible path for physical devices; use when the
//                 sequencer route misbehaves (e.g. events lost in bursts)
//
// Backend-specific setup (which ports/devices to open) stays on the
// concrete classes; StandaloneHost and main.cpp use only this interface
// once a backend is opened.
class MidiInput {
public:
    using MonitorFn = std::function<void(const std::string& portName, const MidiEvent& event)>;

    virtual ~MidiInput() = default;

    virtual void close() = 0;

    // consumer side (audio thread), lock-free
    virtual bool pop(MidiEvent& out) = 0;

    virtual uint64_t receivedCount() const = 0;
    // producer-side losses (a queue was full); should stay 0 in practice
    virtual uint64_t droppedCount() const = 0;
    // complete messages that could not be decoded into a MidiEvent —
    // nonzero values are diagnostic gold when notes go missing
    virtual uint64_t undecodedCount() const = 0;

    // --- debug monitor (--verbose): mirrored events printed by main -----
    virtual void setMonitorEnabled(bool enabled) = 0;
    virtual void drainMonitor(const MonitorFn& fn) = 0;  // main thread

    // one line for the startup log, e.g. backend + port names
    virtual std::string description() const = 0;
};

}  // namespace rtsynth
