#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "../core/Parameters.hpp"
#include "ControlInput.hpp"
#include "ControlLoop.hpp"
#include "LedOutput.hpp"

namespace rtsynth {

// Shape of the envelopes an EgEditPanel edits, and how their parameters
// are named. The defaults describe the pd synth (CZ-style 8-step EGs):
//
//   line<L>_<eg>_rate<K>   K = 1..8
//   line<L>_<eg>_level<K>  K = 1..7
//   line<L>_<eg>_sustain   8 options: Off, step 1..7
//   line<L>_<eg>_end       7 options: step 2..8
struct EgEditSpec {
    int lines = 2;                                     // one encoder row each
    std::vector<std::string> egs = {"dco", "dcw", "dca"};
    int stepsPerRow = 8;                               // encoders per row
    int rateSteps = 8;
    int levelSteps = 7;
    int sustainOptions = 8;   // option 0 = Off, option k = sustain at step k
    int endOptions = 7;       // option k = end at step k + endFirstStep
    int endFirstStep = 2;
};

// The "edit page" logic of a hardware EG editor: rows of encoders, one
// row per line, one encoder per EG step, and buttons that choose WHAT the
// encoders currently edit. For the default spec (pd, 2 x 8 encoders):
//
//   row 1 encoder K  ->  line1_<eg>_<field>K
//   row 2 encoder K  ->  line2_<eg>_<field>K
//
//   buttons (radio)  dco / dcw / dca      which EG          (or "eg": cycle)
//                    rate / level         which field       (or "field": toggle)
//   buttons (hold)   sus + turn encoder K   sustain point of that row's line
//                                           = step K (the last encoder = Off)
//                    end + turn encoder K   end point = step K
//
//   LEDs             dco dcw dca rate level   the current selection
//                    sus end                  lit while held
//                    row<R>.step<K>  (optional) steps 1..end lit; while sus
//                                    or end is held, only that point
//
// The role names are what a panel config file (see PanelConfig) uses to
// attach physical controls; anything not attached is simply absent, so a
// panel with only "eg" and "field" buttons and no LEDs works too.
//
// Runs entirely on the ControlLoop thread (poll() as a ControlSurface,
// onButton() from the loop's button drain), so it needs no locking; the
// parameters it writes are atomic as always.
class EgEditPanel : public ControlSurface {
public:
    enum class Field { Rate, Level };

    explicit EgEditPanel(EgEditSpec spec = {}) : spec_(std::move(spec)){}

    // Resolves every parameter the panel can address. Returns false with
    // the first missing id in `error` (e.g. the instrument is not pd).
    bool bind(ParameterSet& parameters, std::string& error){
        lines_.assign(static_cast<size_t>(spec_.lines), {});
        for(int line = 0; line < spec_.lines; line++){
            LineParams& lp = lines_[static_cast<size_t>(line)];
            lp.egs.resize(spec_.egs.size());
            for(size_t eg = 0; eg < spec_.egs.size(); eg++){
                const std::string prefix =
                    "line" + std::to_string(line + 1) + "_" + spec_.egs[eg] + "_";
                EgParams& ep = lp.egs[eg];
                auto find = [&](const std::string& id) -> Parameter* {
                    Parameter* p = parameters.byId(id);
                    if(p == nullptr && error.empty()){
                        error = "the instrument has no parameter '" + id + "'";
                    }
                    return p;
                };
                for(int k = 1; k <= spec_.rateSteps; k++){
                    ep.rates.push_back(find(prefix + "rate" + std::to_string(k)));
                }
                for(int k = 1; k <= spec_.levelSteps; k++){
                    ep.levels.push_back(find(prefix + "level" + std::to_string(k)));
                }
                ep.sustain = find(prefix + "sustain");
                ep.end = find(prefix + "end");
            }
        }
        return error.empty();
    }

    // normalized change per encoder step (rate / level)
    void setStepSize(float stepSize){ stepSize_ = stepSize; }

    // row and step are 1-based, as on the panel
    bool attachEncoder(int row, int step, RelativeControlInput* input, int channel){
        if(row < 1 || row > spec_.lines || step < 1 || step > spec_.stepsPerRow){
            return false;
        }
        encoders_.push_back({row - 1, step, input, channel});
        return true;
    }

    // "row<R>.<K>" -> (R, K); false when the text is not an encoder role
    static bool parseEncoderRole(const std::string& role, int& row, int& step){
        return parseRowRole(role, "", row, step);
    }

    // Button role -> id for onButton(), or -1 when the panel has no such
    // role (the caller may then treat it as a host action instead).
    int buttonId(const std::string& role) const {
        for(size_t eg = 0; eg < spec_.egs.size(); eg++){
            if(role == spec_.egs[eg]){
                return kButtonEgBase + static_cast<int>(eg);
            }
        }
        if(role == "eg")    return kButtonEgCycle;
        if(role == "rate")  return kButtonRate;
        if(role == "level") return kButtonLevel;
        if(role == "field") return kButtonFieldToggle;
        if(role == "sus")   return kButtonSustain;
        if(role == "end")   return kButtonEnd;
        return -1;
    }

    // Called from the ControlLoop thread.
    void onButton(int id, bool pressed){
        switch(id){
            case kButtonSustain: sustainHeld_ = pressed; return;
            case kButtonEnd:     endHeld_ = pressed; return;
            default: break;
        }
        if(!pressed){
            return;  // the radio buttons act on the press
        }
        const int egCount = static_cast<int>(spec_.egs.size());
        if(id >= kButtonEgBase && id < kButtonEgBase + egCount){
            eg_ = id - kButtonEgBase;
        }else if(id == kButtonEgCycle){
            eg_ = (eg_ + 1) % std::max(1, egCount);
        }else if(id == kButtonRate){
            field_ = Field::Rate;
        }else if(id == kButtonLevel){
            field_ = Field::Level;
        }else if(id == kButtonFieldToggle){
            field_ = (field_ == Field::Rate)? Field::Level : Field::Rate;
        }
    }

    // False for an unknown role.
    bool attachLed(const std::string& role, LedOutput* output, int channel){
        int row = 0, step = 0;
        int kind = -1;
        if(parseRowRole(role, "step", row, step)){
            if(row < 1 || row > spec_.lines || step < 1 || step > spec_.stepsPerRow){
                return false;
            }
            kind = kLedStepBase + (row - 1) * spec_.stepsPerRow + (step - 1);
        }else{
            kind = buttonId(role);
            if(kind < 0 || kind == kButtonEgCycle || kind == kButtonFieldToggle){
                return false;
            }
        }
        leds_.push_back({kind, output, channel, -1});
        return true;
    }

    void poll() override {
        for(EncoderSlot& slot : encoders_){
            const int delta = slot.input->readDelta(slot.channel);
            if(delta == 0){
                continue;
            }
            if(sustainHeld_){
                setSustainStep(slot.row, slot.step);
            }else if(endHeld_){
                setEndStep(slot.row, slot.step);
            }else if(Parameter* p = target(slot.row, slot.step)){
                p->setNormalized(p->getNormalized()
                                 + static_cast<float>(delta) * stepSize_);
            }
        }
        updateLeds();
    }

    // --- state, for the self-test and a future display ---------------------
    int selectedEg() const { return eg_; }
    const std::string& selectedEgName() const { return spec_.egs[static_cast<size_t>(eg_)]; }
    Field selectedField() const { return field_; }

    // the parameter encoder (row, step) edits right now; null for a step
    // without one (level 8 on pd), 0-based row, 1-based step
    Parameter* target(int row, int step) const {
        if(row < 0 || row >= static_cast<int>(lines_.size())){
            return nullptr;
        }
        const EgParams& ep = lines_[static_cast<size_t>(row)].egs[static_cast<size_t>(eg_)];
        const std::vector<Parameter*>& list = (field_ == Field::Rate)? ep.rates : ep.levels;
        return (step >= 1 && step <= static_cast<int>(list.size()))
             ? list[static_cast<size_t>(step - 1)] : nullptr;
    }

    // current sustain / end step of a line's selected EG (0 = sustain off)
    int sustainStep(int row) const {
        const Parameter* p = egParams(row).sustain;
        return p? decodeOption(p->getNormalized(), spec_.sustainOptions) : 0;
    }
    int endStep(int row) const {
        const Parameter* p = egParams(row).end;
        return p? decodeOption(p->getNormalized(), spec_.endOptions) + spec_.endFirstStep : 0;
    }

private:
    static constexpr int kButtonEgCycle = 0;
    static constexpr int kButtonRate = 1;
    static constexpr int kButtonLevel = 2;
    static constexpr int kButtonFieldToggle = 3;
    static constexpr int kButtonSustain = 4;
    static constexpr int kButtonEnd = 5;
    static constexpr int kButtonEgBase = 16;    // + eg index
    static constexpr int kLedStepBase = 1000;   // + row * stepsPerRow + step

    struct EgParams {
        std::vector<Parameter*> rates;
        std::vector<Parameter*> levels;
        Parameter* sustain = nullptr;
        Parameter* end = nullptr;
    };
    struct LineParams {
        std::vector<EgParams> egs;
    };
    struct EncoderSlot {
        int row;    // 0-based
        int step;   // 1-based
        RelativeControlInput* input;
        int channel;
    };
    struct LedSlot {
        int kind;
        LedOutput* output;
        int channel;
        int shown;  // -1 = never written
    };

    // "row<R>.<prefix><K>"
    static bool parseRowRole(const std::string& role, const std::string& prefix,
                             int& row, int& step){
        if(role.rfind("row", 0) != 0){
            return false;
        }
        const size_t dot = role.find('.');
        if(dot == std::string::npos || role.compare(dot + 1, prefix.size(), prefix) != 0){
            return false;
        }
        const std::string rowText = role.substr(3, dot - 3);
        const std::string stepText = role.substr(dot + 1 + prefix.size());
        if(rowText.empty() || stepText.empty()
           || rowText.find_first_not_of("0123456789") != std::string::npos
           || stepText.find_first_not_of("0123456789") != std::string::npos){
            return false;
        }
        row = std::stoi(rowText);
        step = std::stoi(stepText);
        return true;
    }

    // pd's decodeOptionIndex(): equal-width buckets over [0, 1]
    static int decodeOption(float normalized, int numOptions){
        const int index = static_cast<int>(normalized * static_cast<float>(numOptions) + 1e-6f);
        return std::clamp(index, 0, numOptions - 1);
    }
    // ...and the index/(n-1) value a control sends for an option
    static float optionValue(int index, int numOptions){
        return (numOptions > 1)? static_cast<float>(index) / static_cast<float>(numOptions - 1)
                               : 0.0f;
    }

    const EgParams& egParams(int row) const {
        return lines_[static_cast<size_t>(row)].egs[static_cast<size_t>(eg_)];
    }

    void setSustainStep(int row, int step){
        Parameter* p = egParams(row).sustain;
        if(p == nullptr){
            return;
        }
        // the encoders past the last sustainable step all mean "Off"
        const int option = (step < spec_.sustainOptions)? step : 0;
        p->setNormalized(optionValue(option, spec_.sustainOptions));
    }

    void setEndStep(int row, int step){
        Parameter* p = egParams(row).end;
        const int option = step - spec_.endFirstStep;
        if(p == nullptr || option < 0 || option >= spec_.endOptions){
            return;  // no such end point (pd's EG cannot end at step 1)
        }
        p->setNormalized(optionValue(option, spec_.endOptions));
    }

    bool ledWanted(int kind) const {
        if(kind >= kLedStepBase){
            const int index = kind - kLedStepBase;
            const int row = index / spec_.stepsPerRow;
            const int step = index % spec_.stepsPerRow + 1;
            if(sustainHeld_){
                return step == sustainStep(row);
            }
            if(endHeld_){
                return step == endStep(row);
            }
            return step <= endStep(row);   // the steps this EG actually runs
        }
        if(kind >= kButtonEgBase){
            return kind - kButtonEgBase == eg_;
        }
        switch(kind){
            case kButtonRate:    return field_ == Field::Rate;
            case kButtonLevel:   return field_ == Field::Level;
            case kButtonSustain: return sustainHeld_;
            case kButtonEnd:     return endHeld_;
            default:             return false;
        }
    }

    void updateLeds(){
        for(LedSlot& led : leds_){
            const int wanted = ledWanted(led.kind)? 1 : 0;
            if(wanted != led.shown){
                led.output->setLed(led.channel, wanted != 0);
                led.shown = wanted;
            }
        }
    }

    EgEditSpec spec_;
    std::vector<LineParams> lines_;
    std::vector<EncoderSlot> encoders_;
    std::vector<LedSlot> leds_;
    float stepSize_ = 0.01f;
    int eg_ = 0;
    Field field_ = Field::Rate;
    bool sustainHeld_ = false;
    bool endHeld_ = false;
};

}  // namespace rtsynth
