#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ButtonInput.hpp"
#include "ControlInput.hpp"
#include "I2cBus.hpp"
#include "LedOutput.hpp"
#include "QuadratureDecoder.hpp"

namespace rtsynth {

// One pin of one expander: chip = declaration index (addExpander order),
// pin 0..15 = GPA0..GPA7, GPB0..GPB7.
struct ExpanderPin {
    int chip = 0;
    int pin = 0;
};

// A control panel built from MCP23017 16-bit I2C I/O expanders: rotary
// encoders, momentary buttons and LEDs on any expander pins, up to 8
// chips (addresses 0x20-0x27) on one I2C bus. This is how a panel grows
// past the Pi's own GPIO pins — 16 encoders alone need 32 inputs.
//
// The three kinds of control are exposed through the same interfaces as
// their GPIO counterparts, so everything above this class is unaware of
// the expander:
//
//   encoders()  RelativeControlInput   (as GpioEncoderInput)
//   buttons()   ButtonInput            (as GpioButtonInput)
//   leds()      LedOutput
//
// Wiring per chip (3.3 V supply, NOT 5 V — the Pi's I2C is 3.3 V):
//   VDD -> 3V3, VSS -> GND, SCL/SDA -> GPIO3/GPIO2 (shared with the LCD),
//   A0-A2 -> address (0x20 + A2A1A0), RESET -> 3V3 (it must not float).
//   Encoder: A, B -> two expander pins, C -> GND.
//   Button:  one leg -> expander pin, other leg -> GND.
//   (Both use the chip's internal ~100 kOhm pull-ups.)
//   LED:     pin -> resistor -> LED -> GND        (active high, default)
//         or 3V3 -> resistor -> LED -> pin        (active low)
//   INTA (optional): all chips' INTA tied together to one Pi GPIO.
//
// Two traps the configuration check enforces:
//   - GPA7/GPB7 are OUTPUT-ONLY on current MCP23017 silicon (Microchip
//     removed their input function; reading them can corrupt the bus).
//     Put LEDs there, never encoders or buttons.
//   - the common PCF8574 LCD backpack sits at 0x27, which is also an
//     MCP23017 address. Keep the expanders at 0x20-0x26 (or use a 0x3F
//     backpack).
//
// Reading: a scan thread reads GPIOA+GPIOB of every chip that has inputs
// (one 2-byte transaction each), feeds the encoder pins into
// QuadratureDecoders and the button pins into a debouncer. Without an
// interrupt line it scans at a fixed rate (1 kHz by default — enough for
// a hand-turned 24-pulse encoder at several turns per second). With INTA
// wired, the chips are set to open-drain, mirrored INT, and the thread
// sleeps until a pin changes, falling back to a slow full scan (every
// 20 ms) for the debouncer and the LEDs.
//
// INTA can be wired per chip (addExpander's interruptLine — each chip to
// its own Pi GPIO) or shared (Options::interruptLine — all INTAs tied
// together, open-drain makes that safe). Per chip is what scales: only
// the chip whose line fired is read, so a turning encoder costs one 2-byte
// read instead of one per chip. Lines may also be shared by a few chips;
// those are read together. Either every chip with inputs has a line, or
// none does.
//
// LEDs: setLed() only records the wanted level (lock-free); the scan
// thread writes OLAT when it changed, so any thread may drive them.
class Mcp23017Panel {
public:
    struct Options {
        std::string busPath = "/dev/i2c-1";
        // Pi GPIO wired to the chips' INTA, for every chip that names no
        // line of its own (-1 = none: poll at scanRateHz)
        int interruptLine = -1;
        std::string interruptChip = "/dev/gpiochip0";
        int scanRateHz = 1000;
        int quartersPerStep = 4;   // see QuadratureDecoder
        int debounceMs = 5;
        bool pullups = true;       // internal pull-ups on encoder/button pins
    };

    static constexpr int kMaxChips = 8;
    static constexpr int kPinsPerChip = 16;

    Mcp23017Panel() = default;
    ~Mcp23017Panel(){ close(); }

    // --- setup (before open) ------------------------------------------------
    // Each returns the new item's index (= its channel), or -1 on error
    // with the reason in lastError().
    // interruptLine: the Pi GPIO this chip's INTA is wired to (-1 = the
    // shared Options::interruptLine, if any)
    int addExpander(uint8_t address, int interruptLine = -1);
    int addEncoder(ExpanderPin a, ExpanderPin b);
    int addButton(ExpanderPin pin);
    int addLed(ExpanderPin pin, bool activeLow = false);
    const std::string& lastError() const { return error_; }

    // Configures every chip and starts the scan thread.
    bool open(const Options& options);
    void close();

    RelativeControlInput& encoders(){ return encoderBank_; }
    ButtonInput& buttons(){ return buttonBank_; }
    LedOutput& leds(){ return ledBank_; }

    int numExpanders() const { return static_cast<int>(chips_.size()); }
    // expanders that answered at open(); the others are skipped, so a
    // panel can be wired and tried one chip at a time
    int presentExpanders() const {
        int n = 0;
        for(const Chip& chip : chips_) n += chip.present? 1 : 0;
        return n;
    }

    // failed I2C transactions so far (loose wire, chip missing, ...)
    uint64_t readErrorCount() const { return readErrors_.load(std::memory_order_relaxed); }

    // Feeds one chip's port state (GPIOB << 8 | GPIOA) as if just read at
    // time nowMs. The scan thread's only input — public so the self-test
    // can drive the decoding and debouncing without hardware. The first
    // snapshot of a chip only adopts the state (no steps, no edges).
    void processSnapshot(int chip, uint16_t bits, int64_t nowMs);
    // Same setup as open() minus the I/O, for offline use.
    void prepareOffline(const Options& options);

    static std::string pinName(ExpanderPin pin);  // "0.A3"

private:
    struct Chip {
        uint8_t address = 0;
        int interruptLine = -1;   // as declared; -1 = the shared one
        int irqIndex = -1;        // index into irqLines_ once open
        uint16_t inputMask = 0;
        uint16_t outputMask = 0;
        uint16_t bits = 0xFFFF;   // last snapshot (idle = pulled up)
        bool primed = false;
        uint16_t writtenOlat = 0;
        bool olatWritten = false;
        bool present = true;      // answered at open(); absent = skipped
        int silentChanges = 0;    // changes the INT line did not announce
        bool intWarned = false;
    };

    struct Encoder {
        ExpanderPin a, b;
        QuadratureDecoder decoder;
        uint8_t abState = 0;
        std::atomic<int> steps{0};
    };

    struct Button {
        ExpanderPin pin;
        bool pressed = false;
        int64_t lastEdgeMs = -1000000;  // long ago: the first edge is taken
    };

    struct Led {
        ExpanderPin pin;
        bool activeLow = false;
    };

    class EncoderBank : public RelativeControlInput {
    public:
        explicit EncoderBank(Mcp23017Panel& panel) : panel_(panel){}
        const char* name() const override { return "MCP23017 encoders"; }
        int numChannels() const override { return static_cast<int>(panel_.encoders_.size()); }
        int readDelta(int channel) override;
    private:
        Mcp23017Panel& panel_;
    };

    class ButtonBank : public ButtonInput {
    public:
        explicit ButtonBank(Mcp23017Panel& panel) : panel_(panel){}
        const char* name() const override { return "MCP23017 buttons"; }
        int numChannels() const override { return static_cast<int>(panel_.buttons_.size()); }
        void emit(int channel, bool pressed){ publish(channel, pressed); }
    private:
        Mcp23017Panel& panel_;
    };

    class LedBank : public LedOutput {
    public:
        explicit LedBank(Mcp23017Panel& panel) : panel_(panel){}
        int numChannels() const override { return static_cast<int>(panel_.leds_.size()); }
        void setLed(int channel, bool on) override;
    private:
        Mcp23017Panel& panel_;
    };

    bool claimPin(ExpanderPin pin, bool asInput, const char* what);
    bool configureChip(Chip& chip);
    bool openInterrupt();
    void scanThread();
    void scanAll();
    bool scanChip(size_t chip, int64_t now, bool* changed = nullptr);
    uint64_t assertedLines() const;   // bit i = irqLines_[i] is low
    void flushLeds();
    bool pinLevel(ExpanderPin pin) const {
        return (chips_[static_cast<size_t>(pin.chip)].bits >> pin.pin) & 0x1;
    }

    Options options_;
    std::string error_;
    std::vector<Chip> chips_;
    std::vector<std::unique_ptr<Encoder>> encoders_;
    std::vector<Button> buttons_;
    std::vector<Led> leds_;
    // wanted OLAT level of every LED pin, per chip (any thread writes)
    std::array<std::atomic<uint16_t>, kMaxChips> ledLevels_{};

    EncoderBank encoderBank_{*this};
    ButtonBank buttonBank_{*this};
    LedBank ledBank_{*this};

    I2cBus bus_;
    int interruptFd_ = -1;
    std::vector<unsigned int> irqLines_;  // distinct Pi GPIOs requested
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> readErrors_{0};
};

}  // namespace rtsynth
