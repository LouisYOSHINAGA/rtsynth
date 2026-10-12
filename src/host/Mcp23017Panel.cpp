#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/gpio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#include "Mcp23017Panel.hpp"

namespace rtsynth {

namespace {

// MCP23017 registers in the power-on IOCON.BANK = 0 layout, where each
// A register is followed by its B twin — so one 2-byte access starting
// at the A register covers both ports.
constexpr uint8_t kRegIodirA = 0x00;
constexpr uint8_t kRegIpolA = 0x02;
constexpr uint8_t kRegGpintenA = 0x04;
constexpr uint8_t kRegIntconA = 0x08;
constexpr uint8_t kRegIocon = 0x0A;
constexpr uint8_t kRegGppuA = 0x0C;
constexpr uint8_t kRegGpioA = 0x12;
constexpr uint8_t kRegOlatA = 0x14;

// IOCON: INTA/INTB mirrored (either port raises both pins) and open-drain,
// so every chip's INTA can be tied to one Pi GPIO with its pull-up.
constexpr uint8_t kIoconMirror = 0x40;
constexpr uint8_t kIoconOdr = 0x04;

// GPA7 / GPB7 lost their input function in the current MCP23017 silicon
constexpr uint16_t kOutputOnlyPins = (1u << 7) | (1u << 15);

// with the interrupt line wired, still scan this often: it settles
// debounced buttons and flushes LED changes
constexpr int kInterruptFallbackMs = 20;

int64_t nowMs(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool writePair(I2cBus& bus, uint8_t address, uint8_t reg, uint16_t value){
    const uint8_t bytes[2] = {static_cast<uint8_t>(value & 0xFF),
                              static_cast<uint8_t>(value >> 8)};
    return bus.writeRegisters(address, reg, bytes, 2);
}

}  // namespace

std::string Mcp23017Panel::pinName(ExpanderPin pin){
    std::string name = std::to_string(pin.chip);
    name += (pin.pin < 8)? ".A" : ".B";
    name += std::to_string(pin.pin % 8);
    return name;
}

int Mcp23017Panel::addExpander(uint8_t address, int interruptLine){
    if(chips_.size() >= kMaxChips){
        error_ = "at most 8 MCP23017 fit on one bus";
        return -1;
    }
    if(address < 0x20 || address > 0x27){
        error_ = "MCP23017 addresses are 0x20-0x27";
        return -1;
    }
    for(const Chip& chip : chips_){
        if(chip.address == address){
            error_ = "expander address declared twice";
            return -1;
        }
    }
    Chip chip;
    chip.address = address;
    chip.interruptLine = interruptLine;
    chips_.push_back(chip);
    return static_cast<int>(chips_.size()) - 1;
}

bool Mcp23017Panel::claimPin(ExpanderPin pin, bool asInput, const char* what){
    if(pin.chip < 0 || pin.chip >= numExpanders()){
        error_ = std::string(what) + " on expander " + std::to_string(pin.chip)
               + ", but only " + std::to_string(numExpanders()) + " declared";
        return false;
    }
    if(pin.pin < 0 || pin.pin >= kPinsPerChip){
        error_ = std::string(what) + " pin out of range";
        return false;
    }
    Chip& chip = chips_[static_cast<size_t>(pin.chip)];
    const uint16_t bit = static_cast<uint16_t>(1u << pin.pin);
    if(((chip.inputMask | chip.outputMask) & bit) != 0){
        error_ = "pin " + pinName(pin) + " is used twice";
        return false;
    }
    if(asInput && (kOutputOnlyPins & bit) != 0){
        error_ = std::string(what) + " on " + pinName(pin)
               + ": GPA7/GPB7 are output-only on the MCP23017 — use them for LEDs";
        return false;
    }
    (asInput? chip.inputMask : chip.outputMask) |= bit;
    return true;
}

int Mcp23017Panel::addEncoder(ExpanderPin a, ExpanderPin b){
    if(!claimPin(a, true, "encoder") || !claimPin(b, true, "encoder")){
        return -1;
    }
    auto encoder = std::make_unique<Encoder>();
    encoder->a = a;
    encoder->b = b;
    encoders_.push_back(std::move(encoder));
    return static_cast<int>(encoders_.size()) - 1;
}

int Mcp23017Panel::addButton(ExpanderPin pin){
    if(!claimPin(pin, true, "button")){
        return -1;
    }
    Button button;
    button.pin = pin;
    buttons_.push_back(button);
    return static_cast<int>(buttons_.size()) - 1;
}

int Mcp23017Panel::addLed(ExpanderPin pin, bool activeLow){
    if(!claimPin(pin, false, "LED")){
        return -1;
    }
    leds_.push_back({pin, activeLow});
    const int channel = static_cast<int>(leds_.size()) - 1;
    ledBank_.setLed(channel, false);  // establishes the "off" level
    return channel;
}

void Mcp23017Panel::prepareOffline(const Options& options){
    options_ = options;
    for(auto& encoder : encoders_){
        encoder->decoder = QuadratureDecoder(options_.quartersPerStep);
    }
}

bool Mcp23017Panel::open(const Options& options){
    if(chips_.empty()){
        error_ = "no expander declared";
        return false;
    }
    prepareOffline(options);

    // which Pi GPIO serves which chip
    irqLines_.clear();
    bool anyLine = false, missingLine = false;
    for(Chip& chip : chips_){
        const int line = (chip.interruptLine >= 0)? chip.interruptLine : options_.interruptLine;
        chip.irqIndex = -1;
        if(line < 0){
            missingLine |= (chip.inputMask != 0);
            continue;
        }
        anyLine = true;
        auto it = std::find(irqLines_.begin(), irqLines_.end(), static_cast<unsigned int>(line));
        if(it == irqLines_.end()){
            irqLines_.push_back(static_cast<unsigned int>(line));
            it = irqLines_.end() - 1;
        }
        chip.irqIndex = static_cast<int>(it - irqLines_.begin());
    }
    if(anyLine && missingLine){
        error_ = "some expanders have an interrupt line and some do not — "
                 "give every expander with inputs one (or set a shared one)";
        std::cerr << error_ << std::endl;
        return false;
    }
    if(irqLines_.size() > GPIO_V2_LINES_MAX){
        error_ = "too many interrupt lines";
        return false;
    }

    if(!bus_.open(options_.busPath)){
        return false;
    }
    for(Chip& chip : chips_){
        if(!configureChip(chip)){
            close();
            return false;
        }
    }
    if(!openInterrupt()){
        close();
        return false;
    }

    // adopt the knobs' and buttons' current positions before any event
    scanAll();
    flushLeds();

    running_.store(true);
    thread_ = std::thread([this]{ scanThread(); });
    return true;
}

void Mcp23017Panel::close(){
    if(running_.exchange(false)){
        if(thread_.joinable()){
            thread_.join();
        }
    }
    if(bus_.isOpen()){
        // leave the panel dark rather than frozen in its last state
        for(size_t c = 0; c < leds_.size(); c++){
            ledBank_.setLed(static_cast<int>(c), false);
        }
        flushLeds();
    }
    if(interruptFd_ >= 0){
        ::close(interruptFd_);
        interruptFd_ = -1;
    }
    bus_.close();
}

bool Mcp23017Panel::configureChip(Chip& chip){
    const uint8_t a = chip.address;
    // every pin that is not an LED stays an input; unused ones get the
    // pull-up so they do not float
    const uint16_t inputs = static_cast<uint16_t>(~chip.outputMask);
    const uint16_t unused = static_cast<uint16_t>(~(chip.inputMask | chip.outputMask));
    const uint16_t pullups = options_.pullups? inputs : unused;
    const uint16_t interrupts = (chip.irqIndex >= 0)? chip.inputMask : 0;
    const size_t index = static_cast<size_t>(&chip - chips_.data());
    const uint16_t olat = ledLevels_[index].load(std::memory_order_relaxed);

    // OLAT before IODIR, so LED pins come up already "off"
    const bool ok = bus_.writeRegister(a, kRegIocon, kIoconMirror | kIoconOdr)
                 && writePair(bus_, a, kRegOlatA, olat)
                 && writePair(bus_, a, kRegGppuA, pullups)
                 && writePair(bus_, a, kRegIpolA, 0x0000)
                 && writePair(bus_, a, kRegIodirA, inputs)
                 && writePair(bus_, a, kRegIntconA, 0x0000)   // interrupt on any change
                 && writePair(bus_, a, kRegGpintenA, interrupts);
    if(!ok){
        std::cerr << "MCP23017 at 0x" << std::hex << +a << std::dec
                  << " does not answer on " << options_.busPath
                  << " (check wiring, A0-A2, RESET tied high; i2cdetect -y 1)"
                  << std::endl;
        return false;
    }
    chip.writtenOlat = olat;
    chip.olatWritten = true;
    return true;
}

bool Mcp23017Panel::openInterrupt(){
    if(irqLines_.empty()){
        return true;  // polled panel
    }
    const int chipFd = ::open(options_.interruptChip.c_str(), O_RDWR);
    if(chipFd < 0){
        std::cerr << "Failed to open GPIO chip " << options_.interruptChip << std::endl;
        return false;
    }
    // every INT line in one request: one fd to poll, events name their line
    gpio_v2_line_request request{};
    for(size_t i = 0; i < irqLines_.size(); i++){
        request.offsets[i] = irqLines_[i];
    }
    request.num_lines = static_cast<uint32_t>(irqLines_.size());
    // open-drain INT, active low: the Pi's pull-up idles it high
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT
                         | GPIO_V2_LINE_FLAG_EDGE_FALLING
                         | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    std::strncpy(request.consumer, "rtsynth", sizeof(request.consumer) - 1);
    const bool ok = ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request) == 0 && request.fd >= 0;
    ::close(chipFd);
    if(!ok){
        std::cerr << "Failed to request the expander interrupt GPIO line(s)";
        for(unsigned int line : irqLines_){
            std::cerr << " " << line;
        }
        std::cerr << std::endl;
        return false;
    }
    interruptFd_ = request.fd;
    return true;
}

uint64_t Mcp23017Panel::assertedLines() const {
    gpio_v2_line_values values{};
    values.mask = (irqLines_.size() >= 64)? ~0ull : ((1ull << irqLines_.size()) - 1);
    if(ioctl(interruptFd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0){
        return 0;
    }
    return ~values.bits & values.mask;  // active low
}

void Mcp23017Panel::scanAll(){
    const int64_t now = nowMs();
    for(size_t c = 0; c < chips_.size(); c++){
        scanChip(c, now);
    }
}

bool Mcp23017Panel::scanChip(size_t c, int64_t now){
    if(chips_[c].inputMask == 0){
        return true;  // LED-only chip: nothing to read
    }
    uint8_t bytes[2];
    // reading GPIO also clears the chip's interrupt
    if(!bus_.readRegisters(chips_[c].address, kRegGpioA, bytes, 2)){
        readErrors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    processSnapshot(static_cast<int>(c),
                    static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)), now);
    return true;
}

void Mcp23017Panel::flushLeds(){
    for(size_t c = 0; c < chips_.size(); c++){
        Chip& chip = chips_[c];
        if(chip.outputMask == 0){
            continue;
        }
        const uint16_t wanted = ledLevels_[c].load(std::memory_order_relaxed);
        if(chip.olatWritten && wanted == chip.writtenOlat){
            continue;
        }
        if(writePair(bus_, chip.address, kRegOlatA, wanted)){
            chip.writtenOlat = wanted;
            chip.olatWritten = true;
        }else{
            readErrors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Mcp23017Panel::processSnapshot(int chipIndex, uint16_t bits, int64_t now){
    if(chipIndex < 0 || chipIndex >= numExpanders()){
        return;
    }
    Chip& chip = chips_[static_cast<size_t>(chipIndex)];
    const bool first = !chip.primed;
    const uint16_t changed = static_cast<uint16_t>(chip.bits ^ bits);
    chip.bits = bits;
    chip.primed = true;

    // (buttons below are checked even when nothing changed: a contact that
    // settled inside the debounce window is only reported on a later scan)
    for(auto& encoder : encoders_){
        if(!first && changed == 0){
            break;
        }
        if(encoder->a.chip != chipIndex && encoder->b.chip != chipIndex){
            continue;
        }
        const uint8_t ab = static_cast<uint8_t>((pinLevel(encoder->a) << 1)
                                                | pinLevel(encoder->b));
        if(first){
            encoder->abState = ab;
            encoder->decoder.reset(ab);
            continue;
        }
        if(ab == encoder->abState){
            continue;
        }
        encoder->abState = ab;
        const int detents = encoder->decoder.onState(ab);
        if(detents != 0){
            encoder->steps.fetch_add(detents, std::memory_order_relaxed);
        }
    }

    for(size_t i = 0; i < buttons_.size(); i++){
        Button& button = buttons_[i];
        if(button.pin.chip != chipIndex){
            continue;
        }
        // pull-up + switch to GND: a low pin is a closed switch
        const bool pressed = !pinLevel(button.pin);
        if(first){
            button.pressed = pressed;
            continue;
        }
        // Take an edge at once, then ignore the pin for debounceMs: the
        // bounce that follows is swallowed, and the next scan after the
        // window reports whatever the contact finally settled on.
        if(pressed != button.pressed && now - button.lastEdgeMs >= options_.debounceMs){
            button.pressed = pressed;
            button.lastEdgeMs = now;
            buttonBank_.emit(static_cast<int>(i), pressed);
        }
    }
}

void Mcp23017Panel::scanThread(){
    using Clock = std::chrono::steady_clock;

    if(interruptFd_ < 0){
        // polled: every chip at scanRateHz
        const auto period = std::chrono::microseconds(
            1000000 / std::max(1, options_.scanRateHz));
        auto next = Clock::now();
        while(running_.load()){
            next += period;
            const auto now = Clock::now();
            if(next < now){
                next = now;  // fell behind (bus stall): do not try to catch up
            }
            std::this_thread::sleep_until(next);
            scanAll();
            flushLeds();
        }
        return;
    }

    // interrupt-driven: read the chips whose line is low, nothing else
    uint64_t pending = 0;          // lines to serve, bit = irqLines_ index
    int64_t lastFullScan = nowMs();
    while(running_.load()){
        if(pending == 0){
            pollfd fd{interruptFd_, POLLIN, 0};
            if(::poll(&fd, 1, kInterruptFallbackMs) > 0 && (fd.revents & POLLIN)){
                gpio_v2_line_event events[16];
                const ssize_t bytes = ::read(interruptFd_, events, sizeof(events));
                const size_t count = (bytes > 0)? bytes / sizeof(events[0]) : 0;
                for(size_t e = 0; e < count; e++){
                    for(size_t i = 0; i < irqLines_.size(); i++){
                        if(irqLines_[i] == events[e].offset){
                            pending |= 1ull << i;
                        }
                    }
                }
            }
        }

        const int64_t now = nowMs();
        if(now - lastFullScan >= kInterruptFallbackMs){
            // settles debounced buttons, catches anything an edge missed
            scanAll();
            lastFullScan = now;
        }else{
            for(size_t c = 0; c < chips_.size(); c++){
                const int irq = chips_[c].irqIndex;
                if(irq >= 0 && (pending >> irq) & 1){
                    scanChip(c, now);
                }
            }
        }
        flushLeds();

        // A shared line is wired-OR: a chip that changed again while
        // another was being read keeps it low without a new falling edge,
        // so as long as a line is low, serve it again instead of waiting.
        pending = assertedLines();
        if(pending != 0){
            // normally a line that stays low is served at once; the pause
            // only keeps a stuck one (chip unplugged) from eating a core
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
}

int Mcp23017Panel::EncoderBank::readDelta(int channel){
    if(channel < 0 || channel >= numChannels()){
        return 0;
    }
    return panel_.encoders_[static_cast<size_t>(channel)]->steps.exchange(
        0, std::memory_order_relaxed);
}

void Mcp23017Panel::LedBank::setLed(int channel, bool on){
    if(channel < 0 || channel >= numChannels()){
        return;
    }
    const Led& led = panel_.leds_[static_cast<size_t>(channel)];
    const uint16_t bit = static_cast<uint16_t>(1u << led.pin.pin);
    std::atomic<uint16_t>& levels = panel_.ledLevels_[static_cast<size_t>(led.pin.chip)];
    if(on != led.activeLow){
        levels.fetch_or(bit, std::memory_order_relaxed);
    }else{
        levels.fetch_and(static_cast<uint16_t>(~bit), std::memory_order_relaxed);
    }
}

}  // namespace rtsynth
