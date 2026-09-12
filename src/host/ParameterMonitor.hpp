#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../core/Processor.hpp"
#include "ParameterDisplay.hpp"
#include "ParameterWatcher.hpp"

namespace rtsynth {

// Decides what the user should be shown about the running instrument, and
// pushes it to every attached ParameterDisplay. This is the piece both the
// --verbose console output and a future LCD share; the backends only draw.
//
// Three sources are merged into one stream of DisplayLines:
//
//   MIDI CC   noteMidiEvent() asks the instrument which parameter a CC
//             targets, so the reported line carries the CC *and* the tone
//             parameter value it produced — the two are useless apart
//   anything  ParameterWatcher catches every other writer as well (ADC
//             pots, GPIO encoders, --param), so a knob shows up like a CC
//   presets   a Program Change replaces every value at once; the preset
//             name is reported instead of a hundred parameter lines
//
// poll() runs on the UI thread — main's loop today, a display thread once
// an LCD exists — at whatever rate the display can take.
class ParameterMonitor {
public:
    explicit ParameterMonitor(Processor& processor)
        : processor_(processor), watcher_(processor.parameters()){
        if(const PresetBank* bank = processor_.presets()){
            lastPreset_ = bank->current();
            lastRevision_ = bank->revision();
        }
    }

    void addDisplay(ParameterDisplay* display){ displays_.push_back(display); }
    bool hasDisplays() const { return !displays_.empty(); }

    // Records which parameter an incoming CC is about to move, so poll()
    // can show cause and effect on the same line. Call it from the UI
    // thread while draining the MIDI monitor queue.
    void noteMidiEvent(const MidiEvent& event){
        if(event.type != MidiEvent::Type::ControlChange){
            return;
        }
        Parameter* parameter = processor_.parameterForCc(event.data1);
        if(parameter == nullptr){
            return;
        }
        char source[32];
        std::snprintf(source, sizeof(source), "CC %u = %u",
                      static_cast<unsigned>(event.data1),
                      static_cast<unsigned>(event.data2));
        for(auto& [target, text] : pending_){
            if(target == parameter){  // same CC twice in one poll period
                text = source;
                return;
            }
        }
        pending_.emplace_back(parameter, source);
    }

    void poll(){
        if(displays_.empty()){
            pending_.clear();
            return;
        }

        // Watch the revision, not the slot number: a revert reloads the
        // slot it is already on, so the number does not move even though
        // every value did.
        PresetBank* bank = processor_.presets();
        if(bank != nullptr && bank->revision() != lastRevision_){
            lastRevision_ = bank->revision();
            lastPreset_ = bank->current();
            watcher_.resync();  // the whole snapshot changed; report the preset
            pending_.clear();
            for(ParameterDisplay* display : displays_){
                display->showPreset(lastPreset_, bank->name(lastPreset_));
            }
            return;
        }

        watcher_.pollChanges([this](Parameter& parameter){
            report(parameter, takeSource(&parameter));
        });

        // A CC that lands on the value the parameter already holds changes
        // nothing, so the watcher stays silent — report it anyway, so every
        // CC that reaches a parameter is visible with its current value.
        for(const auto& [parameter, source] : pending_){
            report(*parameter, source);
        }
        pending_.clear();
    }

private:
    std::string takeSource(const Parameter* parameter){
        for(size_t i = 0; i < pending_.size(); i++){
            if(pending_[i].first == parameter){
                std::string source = std::move(pending_[i].second);
                pending_.erase(pending_.begin() + static_cast<long>(i));
                return source;
            }
        }
        return {};
    }

    void report(Parameter& parameter, std::string source){
        DisplayLine line;
        line.id = parameter.id();
        line.label = parameter.name();
        line.value = processor_.describeValue(parameter);
        if(line.value.empty()){
            char text[32];
            std::snprintf(text, sizeof(text), "%.4g",
                          static_cast<double>(parameter.get()));
            line.value = text;
            if(!parameter.unit().empty()){
                line.value += " " + parameter.unit();
            }
        }
        line.source = std::move(source);
        for(ParameterDisplay* display : displays_){
            display->showParameter(line);
        }
    }

    Processor& processor_;
    ParameterWatcher watcher_;
    std::vector<ParameterDisplay*> displays_;
    std::vector<std::pair<Parameter*, std::string>> pending_;
    int lastPreset_ = 0;
    uint32_t lastRevision_ = 0;
};

}  // namespace rtsynth
