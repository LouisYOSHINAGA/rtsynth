#pragma once

#include <iostream>
#include <string>

namespace rtsynth {

// One line of "what just changed", already reduced to text so that every
// backend renders the same information. Fields are separate rather than
// pre-joined because a 16x2 character LCD wants to place them itself
// (label on row 1, value on row 2) while a console prints one line.
struct DisplayLine {
    std::string id;      // parameter id, e.g. "line1_dcw_rate1" (--param name)
    std::string label;   // short human name, e.g. "L1 DCW Rate 1"
    std::string value;   // formatted value, e.g. "0.787 (531 ms)"
    std::string source;  // what caused it, e.g. "CC 46 = 100"; empty if not MIDI
};

// A display backend.
//
// This is the seam that keeps the console output of --verbose and a future
// external LCD interchangeable: ParameterMonitor does all the work of
// deciding *what* to show (which parameter changed, which CC caused it,
// which preset is live) and hands finished DisplayLines to every attached
// backend. Adding an LCD therefore means writing this interface only —
// no change to the instrument, the host or main's loop:
//
//   class Hd44780Display : public ParameterDisplay {
//       void showParameter(const DisplayLine& line) override {
//           lcd_.write(0, line.label);
//           lcd_.write(1, line.value);
//       }
//       void showPreset(int index, const std::string& name) override {
//           lcd_.write(0, "PRESET " + std::to_string(index));
//           lcd_.write(1, name);
//       }
//   };
//
// Backends are called from the UI thread only (main's poll loop, or a
// dedicated display thread) — never from the audio or MIDI threads — so
// blocking I2C/SPI writes and allocation are fine here.
class ParameterDisplay {
public:
    virtual ~ParameterDisplay() = default;

    virtual void showParameter(const DisplayLine& line) = 0;
    virtual void showPreset(int index, const std::string& name) = 0;
};

// The --verbose console rendering: the stand-in for the LCD, and the
// reference for what a backend is expected to do with a DisplayLine.
class ConsoleParameterDisplay : public ParameterDisplay {
public:
    void showParameter(const DisplayLine& line) override {
        std::cout << "[param] " << line.label << " = " << line.value;
        if(!line.source.empty()){
            std::cout << "   <- " << line.source;
        }
        std::cout << "   (" << line.id << ")" << std::endl;
    }

    void showPreset(int index, const std::string& name) override {
        std::cout << "[preset] " << index << ": " << name << std::endl;
    }
};

}  // namespace rtsynth
