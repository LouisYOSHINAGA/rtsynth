#pragma once

#include <string>
#include <vector>

#include "ParameterDisplay.hpp"
#include "TextDisplay.hpp"

namespace rtsynth {

// The LCD backend of ParameterMonitor: draws the DisplayLines the monitor
// produces onto a small character display — what changed on the top row,
// its value underneath:
//
//   +----------------+
//   |L1 DCW Rate 1   |   <- line.label (line.id when the synth has no name)
//   |0.787 (531 ms)  |   <- line.value, as --verbose prints it
//   +----------------+
//
// Everything above the character cells is the monitor's job, so the LCD
// shows exactly what the console shows and needs no knowledge of MIDI,
// parameters or presets. TextDisplay decides the hardware (I2cLcd1602 on
// a real synth, a fake in the self-test).
//
// Called from the UI thread only — main's poll loop — so the blocking I2C
// writes underneath are harmless.
class LcdParameterDisplay : public ParameterDisplay {
public:
    explicit LcdParameterDisplay(TextDisplay& display)
        : display_(display), drawn_(static_cast<size_t>(display.rows())){}

    // Static two-line message (startup banner, shutdown note, ...).
    void showStatus(const std::string& top, const std::string& bottom){
        draw(0, top);
        draw(1, bottom);
    }

    void showParameter(const DisplayLine& line) override {
        draw(0, line.label.empty()? line.id : line.label);
        draw(1, line.value);
    }

    void showPreset(int index, const std::string& name) override {
        draw(0, "PRESET " + std::to_string(index));
        draw(1, name);
    }

    // Rows actually pushed to the hardware, for the self-test.
    int writeCount() const { return writes_; }

private:
    // Sending a row costs ~2 ms of I2C, and a knob sweep redraws the value
    // row on every poll while the label above it stays put — so compare
    // against what the display already holds and write only the difference.
    void draw(int row, const std::string& text){
        if(row < 0 || row >= display_.rows()){
            return;
        }
        std::string fitted = text;
        if(fitted.size() > static_cast<size_t>(display_.columns())){
            fitted.resize(static_cast<size_t>(display_.columns()));
        }
        std::string& drawn = drawn_[static_cast<size_t>(row)];
        if(drawn == fitted){
            return;
        }
        drawn = fitted;
        display_.writeLine(row, fitted);
        writes_++;
    }

    TextDisplay& display_;
    std::vector<std::string> drawn_;  // last text written to each row
    int writes_ = 0;
};

}  // namespace rtsynth
