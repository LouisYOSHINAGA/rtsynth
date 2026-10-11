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
//   line_select            4 options: Line 1, Line 2, 1+1', 1+2'
struct EgEditSpec {
    int lines = 2;                                     // one panel row each
    std::vector<std::string> egs = {"dco", "dcw", "dca"};
    int stepsPerRow = 8;                               // encoders per row
    int rateSteps = 8;
    int levelSteps = 7;
    int sustainOptions = 8;   // option 0 = Off, option k = sustain at step k
    int endOptions = 7;       // option k = end at step k + endFirstStep
    int endFirstStep = 2;
    std::string lineSelectId = "line_select";          // "" = no line select
    int lineSelectOptions = 4;
};

// The "edit page" logic of a hardware EG editor. The panel has one row
// per line, and every row is a complete editor of its own — its own EG
// choice, its own rate/level choice, its own sustain and end controls:
//
//   row R (R = 1, 2)
//     encoder K            line<R>_<eg>_<rate|level>K of the row's selection
//     button  eg           DCO -> DCW -> DCA -> DCO      LEDs: dco dcw dca
//             (or dco / dcw / dca to pick one directly)
//     button  field        rate <-> level                LEDs: rate level
//             (or rate / level to pick one directly)
//     button  sus          next sustain point: Off, 1, ..., 7, Off, ...
//                                                        LEDs: sus1..sus7
//                                                        (all dark = Off)
//     button  end          next end point: 2, ..., 8, 2, ...
//                                                        LEDs: end2..end8
//   global
//     button  line         Line 1 -> Line 2 -> 1+1' -> 1+2' -> ...
//                                                        LEDs: line.1..line.4
//
// Role names in a panel config file are these, prefixed with the row:
// "row1.eg", "row2.sus", "row1.sus3" (LED), "row2.4" (encoder 4), and
// "line" / "line.2" for the global ones. Anything not attached is simply
// absent, so a smaller panel (no direct dco button, no end LEDs, ...)
// needs no code change.
//
// The sus/end/line LEDs show the real value at all times — it is read
// back from the parameter, so a preset change or a MIDI CC shows up too.
// Level 8 does not exist in pd: encoder 8 is idle on the level page.
//
// Runs entirely on the ControlLoop thread (poll() as a ControlSurface,
// onButton() from the loop's button drain), so it needs no locking; the
// parameters it writes are atomic as always.
class EgEditPanel : public ControlSurface {
public:
    enum class Field { Rate, Level };

    explicit EgEditPanel(EgEditSpec spec = {}) : spec_(std::move(spec)){
        rows_.resize(static_cast<size_t>(spec_.lines));
    }

    // Resolves every parameter the panel can address. Returns false with
    // the first missing id in `error` (e.g. the instrument is not pd).
    bool bind(ParameterSet& parameters, std::string& error){
        auto find = [&](const std::string& id) -> Parameter* {
            Parameter* p = parameters.byId(id);
            if(p == nullptr && error.empty()){
                error = "the instrument has no parameter '" + id + "'";
            }
            return p;
        };
        for(int line = 0; line < spec_.lines; line++){
            Row& row = rows_[static_cast<size_t>(line)];
            row.egs.assign(spec_.egs.size(), {});
            for(size_t eg = 0; eg < spec_.egs.size(); eg++){
                const std::string prefix =
                    "line" + std::to_string(line + 1) + "_" + spec_.egs[eg] + "_";
                EgParams& ep = row.egs[eg];
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
        if(!spec_.lineSelectId.empty()){
            lineSelect_ = find(spec_.lineSelectId);
        }
        return error.empty();
    }

    // normalized change per encoder step (rate / level)
    void setStepSize(float stepSize){ stepSize_ = stepSize; }

    // --- attaching controls (setup, before the loop starts) ----------------

    // "row<R>.<K>" -> (R, K); false when the text is not an encoder role
    static bool parseEncoderRole(const std::string& role, int& row, int& step){
        std::string rest;
        return splitRowRole(role, row, rest) && parseNumber(rest, step);
    }

    // row and step are 1-based, as on the panel
    bool attachEncoder(int row, int step, RelativeControlInput* input, int channel){
        if(!validRow(row) || step < 1 || step > spec_.stepsPerRow){
            return false;
        }
        encoders_.push_back({row - 1, step, input, channel});
        return true;
    }

    // Button role -> id for onButton(), or -1 when the panel has no such
    // role (the caller may then treat it as a host action instead).
    int buttonId(const std::string& role) const {
        if(role == "line"){
            return kButtonLine;
        }
        int row = 0;
        std::string name;
        if(!splitRowRole(role, row, name) || !validRow(row)){
            return -1;
        }
        int kind = -1;
        for(size_t eg = 0; eg < spec_.egs.size(); eg++){
            if(name == spec_.egs[eg]){
                kind = kButtonEgBase + static_cast<int>(eg);
            }
        }
        if(name == "eg")    kind = kButtonEgCycle;
        if(name == "rate")  kind = kButtonRate;
        if(name == "level") kind = kButtonLevel;
        if(name == "field") kind = kButtonFieldToggle;
        if(name == "sus")   kind = kButtonSustain;
        if(name == "end")   kind = kButtonEnd;
        return (kind < 0)? -1 : (row - 1) * kRowStride + kind;
    }

    // False for an unknown role.
    bool attachLed(const std::string& role, LedOutput* output, int channel){
        LedSlot led{-1, LedKind::LineSelect, 0, output, channel, -1};
        if(role.rfind("line.", 0) == 0){
            if(!parseNumber(role.substr(5), led.index)
               || led.index < 1 || led.index > spec_.lineSelectOptions){
                return false;
            }
            leds_.push_back(led);
            rememberOutput(output);
            return true;
        }
        int row = 0;
        std::string name;
        if(!splitRowRole(role, row, name) || !validRow(row)){
            return false;
        }
        led.row = row - 1;
        if(name == "rate" || name == "level"){
            led.kind = LedKind::Field;
            led.index = (name == "rate")? 0 : 1;
        }else if(name.rfind("sus", 0) == 0 && parseNumber(name.substr(3), led.index)){
            // one LED per sustainable step; Off is "all dark"
            led.kind = LedKind::Sustain;
            if(led.index < 1 || led.index >= spec_.sustainOptions) return false;
        }else if(name.rfind("end", 0) == 0 && parseNumber(name.substr(3), led.index)){
            led.kind = LedKind::End;
            if(led.index < spec_.endFirstStep
               || led.index >= spec_.endFirstStep + spec_.endOptions) return false;
        }else{
            const auto eg = std::find(spec_.egs.begin(), spec_.egs.end(), name);
            if(eg == spec_.egs.end()){
                return false;
            }
            led.kind = LedKind::Eg;
            led.index = static_cast<int>(eg - spec_.egs.begin());
        }
        leds_.push_back(led);
        rememberOutput(output);
        return true;
    }

    // --- runtime (ControlLoop thread) ---------------------------------------

    void onButton(int id, bool pressed){
        if(id == kButtonLine){
            if(pressed){
                cycleLineSelect();
            }
            return;
        }
        if(id < 0){
            return;
        }
        const int rowIndex = id / kRowStride;
        const int kind = id % kRowStride;
        if(rowIndex >= spec_.lines){
            return;
        }
        Row& row = rows_[static_cast<size_t>(rowIndex)];

        if(!pressed){
            return;  // every button acts on the press
        }
        const int egCount = static_cast<int>(spec_.egs.size());
        if(kind >= kButtonEgBase && kind < kButtonEgBase + egCount){
            row.eg = kind - kButtonEgBase;
        }else if(kind == kButtonEgCycle){
            row.eg = (row.eg + 1) % std::max(1, egCount);
        }else if(kind == kButtonRate){
            row.field = Field::Rate;
        }else if(kind == kButtonLevel){
            row.field = Field::Level;
        }else if(kind == kButtonFieldToggle){
            row.field = (row.field == Field::Rate)? Field::Level : Field::Rate;
        }else if(kind == kButtonSustain){
            cycleSustain(rowIndex);
        }else if(kind == kButtonEnd){
            cycleEnd(rowIndex);
        }
    }

    void poll() override {
        for(EncoderSlot& slot : encoders_){
            const int delta = slot.input->readDelta(slot.channel);
            if(delta == 0){
                continue;
            }
            if(Parameter* p = target(slot.row, slot.step)){
                p->setNormalized(p->getNormalized()
                                 + static_cast<float>(delta) * stepSize_);
            }
        }
        updateLeds();
    }

    // --- state, for the self-test and a future display ----------------------
    // (rows are 0-based here, steps 1-based)

    int selectedEg(int row) const { return rows_[static_cast<size_t>(row)].eg; }
    const std::string& selectedEgName(int row) const {
        return spec_.egs[static_cast<size_t>(selectedEg(row))];
    }
    Field selectedField(int row) const { return rows_[static_cast<size_t>(row)].field; }

    // the parameter encoder (row, step) edits right now; null for a step
    // without one (level 8 on pd)
    Parameter* target(int row, int step) const {
        if(row < 0 || row >= spec_.lines){
            return nullptr;
        }
        const Row& r = rows_[static_cast<size_t>(row)];
        const EgParams& ep = r.egs[static_cast<size_t>(r.eg)];
        const std::vector<Parameter*>& list = (r.field == Field::Rate)? ep.rates : ep.levels;
        return (step >= 1 && step <= static_cast<int>(list.size()))
             ? list[static_cast<size_t>(step - 1)] : nullptr;
    }

    // current sustain / end step of a row's selected EG (0 = sustain off)
    int sustainStep(int row) const {
        const Parameter* p = egParams(row).sustain;
        return p? decodeOption(p->getNormalized(), spec_.sustainOptions) : 0;
    }
    int endStep(int row) const {
        const Parameter* p = egParams(row).end;
        return p? decodeOption(p->getNormalized(), spec_.endOptions) + spec_.endFirstStep : 0;
    }
    // 1-based line select option (1 = Line 1 ... 4 = 1+2'), 0 if none
    int lineSelectOption() const {
        return lineSelect_? decodeOption(lineSelect_->getNormalized(),
                                         spec_.lineSelectOptions) + 1 : 0;
    }

private:
    // button ids: row * kRowStride + kind; the global line button apart
    static constexpr int kRowStride = 100;
    static constexpr int kButtonEgCycle = 0;
    static constexpr int kButtonRate = 1;
    static constexpr int kButtonLevel = 2;
    static constexpr int kButtonFieldToggle = 3;
    static constexpr int kButtonSustain = 4;
    static constexpr int kButtonEnd = 5;
    static constexpr int kButtonEgBase = 16;    // + eg index
    static constexpr int kButtonLine = 1000000;

    struct EgParams {
        std::vector<Parameter*> rates;
        std::vector<Parameter*> levels;
        Parameter* sustain = nullptr;
        Parameter* end = nullptr;
    };
    struct Row {
        std::vector<EgParams> egs;
        int eg = 0;
        Field field = Field::Rate;
    };
    struct EncoderSlot {
        int row;    // 0-based
        int step;   // 1-based
        RelativeControlInput* input;
        int channel;
    };
    enum class LedKind { Eg, Field, Sustain, End, LineSelect };
    struct LedSlot {
        int row;        // 0-based, -1 for global
        LedKind kind;
        int index;      // eg index / 0 rate 1 level / step / line option
        LedOutput* output;
        int channel;
        int shown;      // -1 = never written
    };

    bool validRow(int row) const { return row >= 1 && row <= spec_.lines; }

    static bool parseNumber(const std::string& text, int& out){
        if(text.empty() || text.size() > 4
           || text.find_first_not_of("0123456789") != std::string::npos){
            return false;
        }
        out = std::stoi(text);
        return true;
    }

    // "row<R>.<rest>"
    static bool splitRowRole(const std::string& role, int& row, std::string& rest){
        if(role.rfind("row", 0) != 0){
            return false;
        }
        const size_t dot = role.find('.');
        if(dot == std::string::npos || !parseNumber(role.substr(3, dot - 3), row)){
            return false;
        }
        rest = role.substr(dot + 1);
        return !rest.empty();
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
        const Row& r = rows_[static_cast<size_t>(row)];
        return r.egs[static_cast<size_t>(r.eg)];
    }

    void cycleSustain(int row){
        Parameter* p = egParams(row).sustain;
        if(p != nullptr){
            const int next = (decodeOption(p->getNormalized(), spec_.sustainOptions) + 1)
                           % spec_.sustainOptions;
            p->setNormalized(optionValue(next, spec_.sustainOptions));
        }
    }

    void cycleEnd(int row){
        Parameter* p = egParams(row).end;
        if(p != nullptr){
            const int next = (decodeOption(p->getNormalized(), spec_.endOptions) + 1)
                           % spec_.endOptions;
            p->setNormalized(optionValue(next, spec_.endOptions));
        }
    }

    void cycleLineSelect(){
        if(lineSelect_ != nullptr){
            const int next = lineSelectOption() % spec_.lineSelectOptions;  // 1-based + 1
            lineSelect_->setNormalized(optionValue(next, spec_.lineSelectOptions));
        }
    }

    bool ledWanted(const LedSlot& led) const {
        switch(led.kind){
            case LedKind::LineSelect:
                return led.index == lineSelectOption();
            case LedKind::Eg:
                return led.index == selectedEg(led.row);
            case LedKind::Field:
                return led.index == ((selectedField(led.row) == Field::Rate)? 0 : 1);
            case LedKind::Sustain:
                return led.index == sustainStep(led.row);
            case LedKind::End:
                return led.index == endStep(led.row);
        }
        return false;
    }

    void rememberOutput(LedOutput* output){
        if(std::find(outputs_.begin(), outputs_.end(), output) == outputs_.end()){
            outputs_.push_back(output);
        }
    }

    void updateLeds(){
        bool changed = false;
        for(LedSlot& led : leds_){
            const int wanted = ledWanted(led)? 1 : 0;
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

    EgEditSpec spec_;
    std::vector<Row> rows_;
    Parameter* lineSelect_ = nullptr;
    std::vector<EncoderSlot> encoders_;
    std::vector<LedSlot> leds_;
    std::vector<LedOutput*> outputs_;
    float stepSize_ = 0.01f;
};

}  // namespace rtsynth
