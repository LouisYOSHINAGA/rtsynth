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
//   expander 0x20                declares expander 0, 1, 2, ... in order
//   interrupt 4 [/dev/gpiochip0] Pi GPIO wired to every chip's INTA (optional)
//   scan-rate 1000               Hz, when there is no interrupt line
//   encoder-quarters 4           quadrature edges per step (see QuadratureDecoder)
//   encoder-step 0.01            normalized change per step
//   debounce 5                   button debounce, ms
//   pullups on|off               internal pull-ups on encoder/button pins
//
//   encoder <role> <pinA> <pinB>
//   button  <role> <pin>
//   led     <role> <pin> [active-low]
//
// A pin is <expander>.<port><bit>, e.g. 0.A0, 2.B5. The roles say what a
// control does; who understands them is decided by main (EgEditPanel's
// rowR.K / dco / sus / ..., the panel button actions, param:<id>). The
// parser only checks the syntax, so a typo is reported with its line.
struct PanelConfig {
    struct Encoder { std::string role; ExpanderPin a, b; int line = 0; };
    struct Button  { std::string role; ExpanderPin pin; int line = 0; };
    struct Led     { std::string role; ExpanderPin pin; bool activeLow = false; int line = 0; };

    Mcp23017Panel::Options options;
    std::vector<uint8_t> expanders;
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
            }else if(key == "expander" && w.size() == 2){
                config.expanders.push_back(static_cast<uint8_t>(std::stoul(w[1], nullptr, 0)));
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
                PanelConfig::Led l{w[1], {}, false, lineNo};
                if(!pin(w[2], l.pin)){
                    return fail("bad pin (expected e.g. 0.A0)");
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
