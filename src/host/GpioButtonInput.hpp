#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ButtonInput.hpp"

namespace rtsynth {

// Momentary panel buttons (tact switches) on GPIO lines.
//
// Wiring is the same as the rotary encoder's: one leg to the GPIO, the
// other to GND, no external resistor — the line is requested with the
// internal pull-up, so the pin idles high and a press pulls it low.
// A falling edge is therefore a press and a rising edge a release.
//
//     3V3 ──[internal pull-up]── GPIOn ──┬── switch ── GND
//                                        │
//   (optional indicator LED)  3V3 ─[R]─▶├┘
//
// The LED branch hangs off the same node but never carries the logic
// level: with the switch open there is no return path so it stays dark
// and merely adds a second (weak) pull-up, and with the switch closed the
// node is shorted to GND, which both lights the LED and gives the input a
// clean low. Putting the LED *in series* between the GPIO and the switch
// would not work — the pin would only ever be pulled down to the LED's
// forward voltage, which does not read as low.
//
// Debouncing is done by the kernel (GPIO_V2_LINE_ATTR_ID_DEBOUNCE), so
// nothing here has to filter contact bounce; a chip or kernel without
// that support falls back to undebounced edges rather than failing.
//
// Threading: one thread polls every button's line fd and publishes the
// edges into the ButtonInput queue; the ControlLoop thread drains it and
// performs the actions (see ButtonInput for why it must be that thread).
class GpioButtonInput : public ButtonInput {
public:
    // setup only (before open); returns the channel index for this button
    int addButton(unsigned int pin);

    bool open(const std::string& chipPath, unsigned int debounceUs = 5000);
    void close();
    ~GpioButtonInput() override { close(); }

    int numChannels() const override { return static_cast<int>(buttons_.size()); }
    const char* name() const override { return "GPIO buttons"; }
    // false when the kernel would not take the debounce attribute, i.e.
    // the edges arrive raw — worth saying once at startup
    bool debounceActive() const { return debounceActive_; }

private:
    struct Button {
        unsigned int pin = 0;
        int fd = -1;
    };

    void eventThread();

    std::vector<std::unique_ptr<Button>> buttons_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool debounceActive_ = false;
};

}  // namespace rtsynth
