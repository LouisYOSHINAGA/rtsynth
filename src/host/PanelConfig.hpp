#pragma once

#include <cstdint>
#include <fstream>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

#include "Mcp23017Panel.hpp"

namespace rtsynth {

// A hardware panel described in a text file (--panel <file>), so that
// wiring 16 encoders, their buttons and LEDs is a matter of editing one
// file instead of a long command line — and so the panel can grow without
// touching code. One statement per line, '#' starts a comment:
//
//   bus /dev/i2c-1               I2C bus of the expanders
//   expander 0x20 [int 4]        declares expander 0, 1, 2, ... in order;
//                                "int" = the Pi GPIO this chip's INTA is on
//   interrupt 4 [/dev/gpiochip0] Pi GPIO wired to every chip's INTA, for the
//                                chips without their own "int" (optional)
//   scan-rate 1000               Hz, when there is no interrupt line
//   encoder-quarters 4           quadrature edges per step (see QuadratureDecoder)
//   encoder-step 0.01            normalized change per step
//   debounce 5                   button debounce, ms
//   pullups on|off               internal pull-ups on encoder/button pins
//   shift-register /dev/spidev0.1 6
//                                a chain of 6 74HC595 for LEDs (optional)
//
//   encoder <role> <pinA> <pinB>
//   button  <role> <pin>
//   led     <role> <pin> [active-low]
//
// A pin is <expander>.<port><bit>, e.g. 0.A0, 2.B5; an LED may also sit on
// the shift-register chain as sr<chip>.<bit>, e.g. sr2.5 (bit 0 = QA). The
// roles say what a
// control does; who understands them is decided by main (EgEditPanel's
// rowR.K / dco / sus / ..., the panel button actions, param:<id>). The
// parser only checks the syntax, so a typo is reported with its line.
struct PanelConfig {
    struct Encoder { std::string role; ExpanderPin a, b; int line = 0; };
    struct Button  { std::string role; ExpanderPin pin; int line = 0; };
    struct Led {
        std::string role;
        ExpanderPin pin;          // when !onShiftRegister
        int shiftChannel = -1;    // chip * 8 + bit, when onShiftRegister
        bool onShiftRegister = false;
        bool activeLow = false;
        int line = 0;
    };

    Mcp23017Panel::Options options;
    struct Expander { uint8_t address = 0; int interruptLine = -1; };
    std::vector<Expander> expanders;
    std::string shiftDevice;          // "" = no 74HC595 chain
    int shiftChips = 0;
    float encoderStep = 0.01f;
    std::vector<Encoder> encoders;
    std::vector<Button> buttons;
    std::vector<Led> leds;
};

// "2.B5" -> {2, 13}
inline bool parseExpanderPin(const std::string& text, ExpanderPin& out){
    const size_t dot = text.find('.');
    if(dot == std::string::npos || dot == 0 || dot + 3 != text.size()){
        return false;
    }
    const std::string chip = text.substr(0, dot);
    if(chip.find_first_not_of("0123456789") != std::string::npos){
        return false;
    }
    const char port = static_cast<char>(text[dot + 1] & ~0x20);  // upper case
    const char bit = text[dot + 2];
    if((port != 'A' && port != 'B') || bit < '0' || bit > '7'){
        return false;
    }
    out.chip = std::stoi(chip);
    out.pin = (port == 'B'? 8 : 0) + (bit - '0');
    return true;
}

// "sr2.5" -> 21
inline bool parseShiftPin(const std::string& text, int& channel){
    if(text.size() < 4 || text.compare(0, 2, "sr") != 0){
        return false;
    }
    const size_t dot = text.find('.');
    if(dot == std::string::npos || dot + 2 != text.size() || dot == 2){
        return false;
    }
    const std::string chip = text.substr(2, dot - 2);
    const char bit = text[dot + 1];
    if(chip.find_first_not_of("0123456789") != std::string::npos || bit < '0' || bit > '7'){
        return false;
    }
    channel = std::stoi(chip) * 8 + (bit - '0');
    return true;
}

// Returns false with "line N: ..." in error.
inline bool parsePanelConfig(std::istream& in, PanelConfig& config, std::string& error){
    std::string text;
    int lineNo = 0;
    while(std::getline(in, text)){
        lineNo++;
        const size_t hash = text.find('#');
        if(hash != std::string::npos){
            text.resize(hash);
        }
        std::istringstream words(text);
        std::vector<std::string> w;
        for(std::string word; words >> word; ){
            w.push_back(word);
        }
        if(w.empty()){
            continue;
        }
        auto fail = [&](const std::string& message){
            error = "line " + std::to_string(lineNo) + ": " + message;
            return false;
        };
        auto pin = [&](const std::string& word, ExpanderPin& out){
            return parseExpanderPin(word, out);
        };
        const std::string& key = w[0];
        try{
            if(key == "bus" && w.size() == 2){
                config.options.busPath = w[1];
            }else if(key == "expander" && (w.size() == 2 || (w.size() == 4 && w[2] == "int"))){
                PanelConfig::Expander e;
                e.address = static_cast<uint8_t>(std::stoul(w[1], nullptr, 0));
                if(w.size() == 4){
                    e.interruptLine = std::stoi(w[3]);
                }
                config.expanders.push_back(e);
            }else if(key == "interrupt" && (w.size() == 2 || w.size() == 3)){
                config.options.interruptLine = std::stoi(w[1]);
                if(w.size() == 3){
                    config.options.interruptChip = w[2];
                }
            }else if(key == "scan-rate" && w.size() == 2){
                config.options.scanRateHz = std::stoi(w[1]);
            }else if(key == "encoder-quarters" && w.size() == 2){
                config.options.quartersPerStep = std::stoi(w[1]);
            }else if(key == "encoder-step" && w.size() == 2){
                config.encoderStep = std::stof(w[1]);
            }else if(key == "debounce" && w.size() == 2){
                config.options.debounceMs = std::stoi(w[1]);
            }else if(key == "pullups" && w.size() == 2 && (w[1] == "on" || w[1] == "off")){
                config.options.pullups = (w[1] == "on");
            }else if(key == "shift-register" && w.size() == 3){
                config.shiftDevice = w[1];
                config.shiftChips = std::stoi(w[2]);
                if(config.shiftChips < 1){
                    return fail("a shift-register chain needs at least one chip");
                }
            }else if(key == "encoder" && w.size() == 4){
                PanelConfig::Encoder e{w[1], {}, {}, lineNo};
                if(!pin(w[2], e.a) || !pin(w[3], e.b)){
                    return fail("bad pin (expected e.g. 0.A0)");
                }
                config.encoders.push_back(e);
            }else if(key == "button" && w.size() == 3){
                PanelConfig::Button b{w[1], {}, lineNo};
                if(!pin(w[2], b.pin)){
                    return fail("bad pin (expected e.g. 0.A0)");
                }
                config.buttons.push_back(b);
            }else if(key == "led" && (w.size() == 3 || w.size() == 4)){
                PanelConfig::Led l;
                l.role = w[1];
                l.line = lineNo;
                if(parseShiftPin(w[2], l.shiftChannel)){
                    l.onShiftRegister = true;
                }else if(!pin(w[2], l.pin)){
                    return fail("bad pin (expected e.g. 0.A0 or sr0.3)");
                }
                if(w.size() == 4){
                    if(w[3] != "active-low"){
                        return fail("unknown LED option '" + w[3] + "' (only: active-low)");
                    }
                    l.activeLow = true;
                }
                config.leds.push_back(l);
            }else{
                return fail("cannot read '" + text + "'");
            }
        }catch(const std::exception&){
            return fail("bad number in '" + text + "'");
        }
    }
    if(config.expanders.empty()){
        error = "no 'expander' declared";
        return false;
    }
    std::vector<bool> shiftUsed(static_cast<size_t>(config.shiftChips) * 8, false);
    for(const PanelConfig::Led& l : config.leds){
        if(l.onShiftRegister && l.shiftChannel < config.shiftChips * 8){
            if(shiftUsed[static_cast<size_t>(l.shiftChannel)]){
                error = "line " + std::to_string(l.line) + ": pin "
                      + "sr" + std::to_string(l.shiftChannel / 8) + "."
                      + std::to_string(l.shiftChannel % 8) + " is used twice";
                return false;
            }
            shiftUsed[static_cast<size_t>(l.shiftChannel)] = true;
        }
        if(l.onShiftRegister && l.shiftChannel >= config.shiftChips * 8){
            error = "line " + std::to_string(l.line) + ": LED on "
                  + "sr" + std::to_string(l.shiftChannel / 8)
                  + ", but the shift-register chain has "
                  + std::to_string(config.shiftChips) + " chip(s)";
            return false;
        }
    }
    return true;
}

inline bool loadPanelConfig(const std::string& path, PanelConfig& config, std::string& error){
    std::ifstream file(path);
    if(!file){
        error = "cannot open " + path;
        return false;
    }
    if(!parsePanelConfig(file, config, error)){
        error = path + ", " + error;
        return false;
    }
    return true;
}

}  // namespace rtsynth
