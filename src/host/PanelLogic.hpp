#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "../core/Parameters.hpp"
#include "ControlInput.hpp"
#include "ControlLoop.hpp"
#include "LedOutput.hpp"

namespace rtsynth {

enum class LedState { Off, On, Blink };

// The behaviour of a hardware panel: what each button, encoder and LED
// *means*. A panel config file names every physical control by a role
// ("row1.eg", "tone3", "abs.up", ...); PanelLogic resolves the role to a
// handler, and the hardware layer (Mcp23017Panel, ShiftRegisterLeds) never
// learns what any of it does.
//
// The meaning comes from Modules — one per feature (the EG editor, the
// tone selector, ...; see PdPanel.hpp). A module answers the roles it
// knows with small functions; PanelLogic asks each module in turn, keeps
// the answers, and runs them:
//
//   button role  -> ButtonFn(pressed)      called from the button drain
//   encoder role -> EncoderFn(delta)       called with each poll's steps
//   LED role     -> LedFn() -> Off/On/Blink, evaluated every poll; only
//                   changes reach the hardware, then one flush() per output
//
// Shared services modules use instead of writing parameters themselves:
//
//   nudge(p, delta)  move by `delta` detents: one option per detent on a
//                    discrete parameter (Parameter::steps), stepSize on a
//                    continuous one — and make p the abs target
//   select(p)        make p the abs target and have the LCD show it
//                    (Parameter::announce) without changing its value
//
// The abs controls (buttons "abs.up" / "abs.down", encoder "abs") nudge
// the abs target: whatever the panel last touched, i.e. what the LCD is
// showing. That is what lets one pair of buttons set every discrete value
// that has no knob of its own (key follow, ...).
//
// Built in besides abs, for controls that need no module:
//   encoder param:<id>          nudges that parameter
//   button  param:<id>          steps a discrete parameter to its next
//                               option (wrapping) — a toggle for 2 options
//   LED     param:<id>=<n>      lit while the parameter is on option n
//
// Everything runs on the ControlLoop thread (poll() is a ControlSurface,
// button handlers come from the loop's drain), so nothing here locks; the
// parameters it writes are atomic as always.
class PanelLogic : public ControlSurface {
public:
    using ButtonFn = std::function<void(bool pressed)>;
    using EncoderFn = std::function<void(int delta)>;
    using LedFn = std::function<LedState()>;

    class Module {
    public:
        virtual ~Module() = default;
        // Empty function = not one of this module's roles.
        virtual ButtonFn button(const std::string& /*role*/){ return {}; }
        virtual EncoderFn encoder(const std::string& /*role*/){ return {}; }
        virtual LedFn led(const std::string& /*role*/){ return {}; }
        // once per poll, before the LEDs are evaluated
        virtual void tick(){}
        // Why the module cannot do its job ("" = it can): its controls
        // still resolve, but do nothing, and setup reports this once.
        virtual std::string unavailable() const { return {}; }
    };

    // Blink period in polls: the ControlLoop polls at 100 Hz, so 25 on +
    // 25 off is a 2 Hz blink.
    static constexpr int kBlinkTicks = 25;

    explicit PanelLogic(ParameterSet& parameters) : parameters_(parameters){}

    void addModule(std::unique_ptr<Module> module){ modules_.push_back(std::move(module)); }

    // normalized change per detent of a continuous parameter
    void setStepSize(float stepSize){ stepSize_ = stepSize; }

    // --- setup: resolve roles (before the loop starts) -----------------------

    // Empty function = no such role.
    ButtonFn button(const std::string& role){
        if(role == "abs.up" || role == "abs.down"){
            const int delta = (role == "abs.up")? +1 : -1;
            return [this, delta](bool pressed){
                if(pressed && target_ != nullptr){
                    nudge(target_, delta);
                }
            };
        }
        if(Parameter* p = paramRole(role)){
            return [this, p](bool pressed){
                if(pressed){
                    step(p);
                }
            };
        }
        return ask([&](Module& m){ return m.button(role); });
    }

    // label: how a trace names this encoder (default: its role)
    bool attachEncoder(const std::string& role, RelativeControlInput* input, int channel,
                       std::string label = {}){
        EncoderFn fn;
        if(role == "abs"){
            fn = [this](int delta){
                if(target_ != nullptr){
                    nudge(target_, delta);
                }
            };
        }else if(Parameter* p = paramRole(role)){
            fn = [this, p](int delta){ nudge(p, delta); };
        }else{
            fn = ask([&](Module& m){ return m.encoder(role); });
        }
        if(!fn){
            return false;
        }
        encoders_.push_back({input, channel, std::move(fn), label.empty()? role : label});
        return true;
    }

    bool attachLed(const std::string& role, LedOutput* output, int channel){
        LedFn fn;
        const size_t eq = role.find('=');
        Parameter* p = (eq != std::string::npos)? paramRole(role.substr(0, eq)) : nullptr;
        int option = 0;
        if(p != nullptr && parseInt(role.substr(eq + 1), option)){
            fn = [p, option]{ return (p->option() == option)? LedState::On : LedState::Off; };
        }else{
            fn = ask([&](Module& m){ return m.led(role); });
        }
        if(!fn){
            return false;
        }
        leds_.push_back({output, channel, std::move(fn), -1});
        if(std::find(outputs_.begin(), outputs_.end(), output) == outputs_.end()){
            outputs_.push_back(output);
        }
        return true;
    }

    // modules that were asked for a role but cannot work, for a startup note
    const std::vector<std::string>& warnings() const { return warnings_; }

    // --- services for modules ------------------------------------------------

    void nudge(Parameter* p, int delta){
        if(p == nullptr){
            return;
        }
        if(p->steps() >= 2){
            p->setOption(p->option() + delta);
        }else{
            p->setNormalized(p->getNormalized() + static_cast<float>(delta) * stepSize_);
        }
        target_ = p;
    }

    // next option of a discrete parameter, wrapping (a toggle for 2)
    void step(Parameter* p){
        if(p == nullptr || p->steps() < 2){
            return;
        }
        p->setOption((p->option() + 1) % p->steps());
        target_ = p;
    }

    void select(Parameter* p){
        if(p != nullptr){
            target_ = p;
            p->announce();
        }
    }

    Parameter* target() const { return target_; }

    // Bring-up aid: called (from the ControlLoop thread) with a line per
    // encoder movement, e.g. "encoder row1.3 (0.A4/0.A5) +2".
    void setTrace(std::function<void(const std::string&)> trace){ trace_ = std::move(trace); }
    bool blinkOn() const { return (ticks_ / kBlinkTicks) % 2 == 0; }

    // --- runtime -------------------------------------------------------------

    void poll() override {
        for(EncoderSlot& slot : encoders_){
            const int delta = slot.input->readDelta(slot.channel);
            if(delta != 0){
                if(trace_){
                    trace_("encoder " + slot.label + " " + (delta > 0? "+" : "")
                           + std::to_string(delta));
                }
                slot.fn(delta);
            }
        }
        for(auto& module : modules_){
            module->tick();
        }
        ticks_++;
        bool changed = false;
        for(LedSlot& led : leds_){
            const LedState state = led.fn();
            const int wanted = (state == LedState::On
                                || (state == LedState::Blink && blinkOn()))? 1 : 0;
            if(wanted != led.shown){
                led.output->setLed(led.channel, wanted != 0);
                led.shown = wanted;
                changed = true;
            }
        }
        if(changed){
            for(LedOutput* output : outputs_){
                output->flush();   // one transfer per output per tick
            }
        }
    }

    // --- helpers shared with modules -----------------------------------------

    static bool parseInt(const std::string& text, int& out){
        if(text.empty() || text.size() > 6
           || text.find_first_not_of("0123456789") != std::string::npos){
            return false;
        }
        out = std::stoi(text);
        return true;
    }

private:
    struct EncoderSlot {
        RelativeControlInput* input;
        int channel;
        EncoderFn fn;
        std::string label;
    };
    struct LedSlot {
        LedOutput* output;
        int channel;
        LedFn fn;
        int shown;  // -1 = never written
    };

    Parameter* paramRole(const std::string& role){
        return (role.rfind("param:", 0) == 0)? parameters_.byId(role.substr(6)) : nullptr;
    }

    // first module that answers; note a module that answers but cannot work
    template <typename Fn>
    std::invoke_result_t<Fn, Module&> ask(Fn&& query){
        for(auto& module : modules_){
            auto fn = query(*module);
            if(fn){
                const std::string why = module->unavailable();
                if(!why.empty()
                   && std::find(warnings_.begin(), warnings_.end(), why) == warnings_.end()){
                    warnings_.push_back(why);
                }
                return fn;
            }
        }
        return {};
    }

    ParameterSet& parameters_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::vector<EncoderSlot> encoders_;
    std::vector<LedSlot> leds_;
    std::vector<LedOutput*> outputs_;
    std::vector<std::string> warnings_;
    Parameter* target_ = nullptr;
    std::function<void(const std::string&)> trace_;
    float stepSize_ = 0.01f;
    long ticks_ = 0;
};

}  // namespace rtsynth
