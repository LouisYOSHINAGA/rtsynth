#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../core/Parameters.hpp"
#include "ParameterWatcher.hpp"
#include "TextDisplay.hpp"

namespace rtsynth {

// One-line value rendering for a character display, e.g. "0.500 s".
// Kept as a free function so the self-test can check it without hardware.
inline std::string formatParameterValue(const Parameter& parameter){
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(parameter.get()));
    std::string text = buffer;
    if(!parameter.unit().empty()){
        text += " ";
        text += parameter.unit();
    }
    return text;
}

// The display thread of the hardware UI: watches the ParameterSet through
// a ParameterWatcher and shows the most recently changed parameter — id on
// the top row, value on the bottom row — whatever wrote it (MIDI CC, ADC
// pot, GPIO encoder, --param). Completes the chain begun by
// Parameter::changeCount()/ParameterWatcher:
//
//   any writer -> atomic Parameter -> ParameterWatcher -> DisplayUi -> LCD
//
// The TextDisplay implementation decides the actual hardware (I2cLcd1602
// for a 16x2 LCD; a fake in the self-test). All drawing happens on this
// class's own thread — never on the audio or MIDI threads.
class DisplayUi {
public:
    DisplayUi(TextDisplay& display, ParameterSet& parameters)
        : display_(display), watcher_(parameters){}
    ~DisplayUi(){ stop(); }

    // Static two-line message (startup banner, shutdown note, ...).
    void showStatus(const std::string& top, const std::string& bottom){
        display_.writeLine(0, top);
        display_.writeLine(1, bottom);
    }

    void start(int refreshHz = 10){
        if(running_.exchange(true)){
            return;
        }
        thread_ = std::thread([this, refreshHz]{
            const auto period =
                std::chrono::milliseconds(1000 / std::max(1, refreshHz));
            while(running_.load()){
                pollOnce();
                std::this_thread::sleep_for(period);
            }
        });
    }

    void stop(){
        if(!running_.exchange(false)){
            return;
        }
        if(thread_.joinable()){
            thread_.join();
        }
    }

    // One poll/redraw pass (also used by the self-test). Redraws only when
    // a parameter actually changed, so an idle synth causes no I2C traffic.
    void pollOnce(){
        Parameter* latest = nullptr;
        watcher_.pollChanges([&](Parameter& p){ latest = &p; });
        if(latest != nullptr){
            display_.writeLine(0, latest->id());
            display_.writeLine(1, formatParameterValue(*latest));
        }
    }

private:
    TextDisplay& display_;
    ParameterWatcher watcher_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace rtsynth
