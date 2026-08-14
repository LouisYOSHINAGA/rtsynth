#pragma once

#include <cstdint>
#include <vector>

#include "../core/Parameters.hpp"

namespace rtsynth {

// Change detector for a UI thread (LCD, OLED, console, ...): polls the
// ParameterSet's per-parameter change counters and reports which
// parameters were written since the last poll — regardless of whether the
// writer was MIDI CC (audio thread), a pot or encoder (control thread) or
// --param (main thread). Writers stay lock-free; the UI polls at whatever
// rate the display can handle.
//
// Typical LCD wiring: a display thread calls pollChanges() at 10-20 Hz
// and redraws the last-changed parameter's name and value:
//
//   ParameterWatcher watcher(synth.parameters());
//   while(uiRunning){
//       watcher.pollChanges([&](Parameter& p){
//           lcd.show(p.name(), p.get(), p.unit());
//       });
//       sleep(50ms);
//   }
//
// (The -v console output in main.cpp uses exactly this mechanism.)
class ParameterWatcher {
public:
    explicit ParameterWatcher(ParameterSet& parameters) : parameters_(parameters){
        lastCounts_.resize(parameters_.size());
        resync();  // adopt current counts so construction doesn't report a burst
    }

    // Adopt the current counts without reporting anything. Used when the
    // whole snapshot was replaced at once (a preset recall writes every
    // parameter), where listing all of them would be noise rather than
    // information.
    void resync(){
        for(size_t i = 0; i < parameters_.size(); i++){
            lastCounts_[i] = parameters_[i].changeCount();
        }
    }

    // Invokes fn(Parameter&) once per parameter changed since last poll.
    template <typename Fn>
    void pollChanges(Fn&& fn){
        for(size_t i = 0; i < parameters_.size(); i++){
            const uint32_t count = parameters_[i].changeCount();
            if(count != lastCounts_[i]){
                lastCounts_[i] = count;
                fn(parameters_[i]);
            }
        }
    }

private:
    ParameterSet& parameters_;
    std::vector<uint32_t> lastCounts_;
};

}  // namespace rtsynth
