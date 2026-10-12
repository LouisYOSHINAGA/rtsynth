#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "../core/Parameters.hpp"
#include "../core/PresetBank.hpp"
#include "PanelLogic.hpp"

namespace rtsynth {

// The panel modules of the pd (CZ-style) synth. Each one is a feature of
// the front panel; PanelLogic resolves the roles of a panel config file
// to them. Parameter ids are pd's (PdSynthProcessor::paramIdString), so a
// module whose parameters are missing (another instrument, or a pd
// without vibrato yet) still accepts its roles but does nothing, and says
// so once at startup.

namespace pdpanel {

// "row<R>.<rest>" -> R (1-based), rest; false otherwise
inline bool splitRow(const std::string& role, int& row, std::string& rest){
    if(role.rfind("row", 0) != 0){
        return false;
    }
    const size_t dot = role.find('.');
    if(dot == std::string::npos || !PanelLogic::parseInt(role.substr(3, dot - 3), row)){
        return false;
    }
    rest = role.substr(dot + 1);
    return !rest.empty();
}

// "<prefix><n>" -> n
inline bool numbered(const std::string& text, const std::string& prefix, int& n){
    return text.rfind(prefix, 0) == 0 && PanelLogic::parseInt(text.substr(prefix.size()), n);
}

}  // namespace pdpanel

// One row of the panel per line: the line's EG editor, waveforms and key
// follow. Roles (R = 1, 2 = the line):
//
//   buttons  rowR.eg         DCx: DCO -> DCW -> DCA -> DCO   (rowR.dco/dcw/dca: direct)
//            rowR.field      rate <-> level                  (rowR.rate/level: direct)
//            rowR.sus        sustain point: Off, 1..7, Off, ...
//            rowR.end        end point: 2..8, 2, ...
//            rowR.wave1      first waveform: 1..8, 1, ...
//            rowR.wave2      second waveform: Off, 1..8, Off, ...
//            rowR.keyfollow  put the DCW or DCA key follow (0..9) on the
//                            LCD and under abs.up/down: the one of the
//                            selected DCx (DCW for DCO); pressing again
//                            swaps DCW <-> DCA
//   encoders rowR.1 .. rowR.8   rate / level K of the selected DCx (no
//                               level 8 in pd: idle on the level page)
//   LEDs     rowR.dco/dcw/dca, rowR.rate/level   the selection
//            rowR.step1..8   the sustain and the end step of the selected
//                            DCx (2 lit, or 1 when sustain is Off)
//            rowR.wave1.bit0..  the waveform number in binary, bit0 =
//            rowR.wave2.bit0..  least significant (wave2 Off = all dark).
//                            A number too big for the LEDs wired (8 on
//                            three LEDs) blinks them all.
//
// sus / end / waveform buttons step the value and leave it as the abs
// target, so abs.up/down can also step it backwards.
class PdLineEditor : public PanelLogic::Module {
public:
    static constexpr int kSteps = 8;

    PdLineEditor(PanelLogic& panel, ParameterSet& parameters, int lines = 2)
        : panel_(panel), rows_(static_cast<size_t>(lines)){
        static const char* const egs[3] = {"dco", "dcw", "dca"};
        for(int line = 0; line < lines; line++){
            Row& row = rows_[static_cast<size_t>(line)];
            const std::string prefix = "line" + std::to_string(line + 1) + "_";
            for(int eg = 0; eg < 3; eg++){
                const std::string egPrefix = prefix + egs[eg] + "_";
                EgParams& ep = row.egs[static_cast<size_t>(eg)];
                for(int k = 1; k <= 8; k++){
                    ep.rates.push_back(find(parameters, egPrefix + "rate" + std::to_string(k)));
                }
                for(int k = 1; k <= 7; k++){
                    ep.levels.push_back(find(parameters, egPrefix + "level" + std::to_string(k)));
                }
                ep.sustain = find(parameters, egPrefix + "sustain");
                ep.end = find(parameters, egPrefix + "end");
            }
            row.wave1 = find(parameters, prefix + "wave1");
            row.wave2 = find(parameters, prefix + "wave2");
            row.keyFollowDcw = find(parameters, prefix + "dcw_key_follow");
            row.keyFollowDca = find(parameters, prefix + "dca_key_follow");
        }
    }

    std::string unavailable() const override {
        return missing_.empty()? std::string()
             : "line editor: the instrument has no parameter '" + missing_
               + "' (it needs --synth pd); those controls do nothing";
    }

    PanelLogic::ButtonFn button(const std::string& role) override {
        int r = 0;
        std::string name;
        if(!pdpanel::splitRow(role, r, name) || !validRow(r)){
            return {};
        }
        const size_t i = static_cast<size_t>(r - 1);
        auto onPress = [](std::function<void()> fn) -> PanelLogic::ButtonFn {
            return [fn](bool pressed){ if(pressed) fn(); };
        };
        Row* row = &rows_[i];
        for(int eg = 0; eg < 3; eg++){
            if(name == kEgNames[eg]){
                return onPress([row, eg]{ row->eg = eg; });
            }
        }
        if(name == "eg")    return onPress([row]{ row->eg = (row->eg + 1) % 3; });
        if(name == "field") return onPress([row]{ row->level = !row->level; });
        if(name == "rate")  return onPress([row]{ row->level = false; });
        if(name == "level") return onPress([row]{ row->level = true; });
        if(name == "sus")   return onPress([this, row]{ panel_.step(row->selected().sustain); });
        if(name == "end")   return onPress([this, row]{ panel_.step(row->selected().end); });
        if(name == "wave1") return onPress([this, row]{ panel_.step(row->wave1); });
        if(name == "wave2") return onPress([this, row]{ panel_.step(row->wave2); });
        if(name == "keyfollow") return onPress([this, row]{ selectKeyFollow(*row); });
        return {};
    }

    PanelLogic::EncoderFn encoder(const std::string& role) override {
        int r = 0, k = 0;
        std::string name;
        if(!pdpanel::splitRow(role, r, name) || !validRow(r)
           || !PanelLogic::parseInt(name, k) || k < 1 || k > kSteps){
            return {};
        }
        Row* row = &rows_[static_cast<size_t>(r - 1)];
        return [this, row, k](int delta){ panel_.nudge(row->target(k), delta); };
    }

    PanelLogic::LedFn led(const std::string& role) override {
        int r = 0, n = 0;
        std::string name;
        if(!pdpanel::splitRow(role, r, name) || !validRow(r)){
            return {};
        }
        Row* row = &rows_[static_cast<size_t>(r - 1)];
        auto on = [](bool lit){ return lit? LedState::On : LedState::Off; };
        for(int eg = 0; eg < 3; eg++){
            if(name == kEgNames[eg]){
                return [row, eg, on]{ return on(row->eg == eg); };
            }
        }
        if(name == "rate")  return [row, on]{ return on(!row->level); };
        if(name == "level") return [row, on]{ return on(row->level); };
        if(pdpanel::numbered(name, "step", n) && n >= 1 && n <= kSteps){
            return [row, n, on]{ return on(n == row->sustainStep() || n == row->endStep()); };
        }
        for(int wave = 1; wave <= 2; wave++){
            const std::string prefix = "wave" + std::to_string(wave) + ".bit";
            if(pdpanel::numbered(name, prefix, n) && n < 8){
                int& bits = (wave == 1)? row->wave1Bits : row->wave2Bits;
                bits = std::max(bits, n + 1);
                const int* width = &bits;
                return [row, wave, n, width]{
                    const int value = row->waveNumber(wave);
                    if(value >= (1 << *width)){
                        return LedState::Blink;   // does not fit the LEDs wired
                    }
                    return ((value >> n) & 1)? LedState::On : LedState::Off;
                };
            }
        }
        return {};
    }

    // --- state, for the self-test --------------------------------------------
    int selectedEg(int row) const { return rows_[static_cast<size_t>(row)].eg; }
    bool levelPage(int row) const { return rows_[static_cast<size_t>(row)].level; }

private:
    static constexpr const char* kEgNames[3] = {"dco", "dcw", "dca"};

    struct EgParams {
        std::vector<Parameter*> rates;
        std::vector<Parameter*> levels;
        Parameter* sustain = nullptr;
        Parameter* end = nullptr;
    };

    struct Row {
        std::array<EgParams, 3> egs;
        Parameter* wave1 = nullptr;
        Parameter* wave2 = nullptr;
        Parameter* keyFollowDcw = nullptr;
        Parameter* keyFollowDca = nullptr;
        int eg = 0;           // 0 DCO, 1 DCW, 2 DCA
        bool level = false;   // false = rate page
        int wave1Bits = 0;    // LEDs wired per waveform display
        int wave2Bits = 0;

        EgParams& selected(){ return egs[static_cast<size_t>(eg)]; }
        const EgParams& selected() const { return egs[static_cast<size_t>(eg)]; }

        Parameter* target(int k) const {
            const std::vector<Parameter*>& list = level? selected().levels : selected().rates;
            return (k >= 1 && k <= static_cast<int>(list.size()))
                 ? list[static_cast<size_t>(k - 1)] : nullptr;
        }
        // pd: sustain option 0 = Off, k = step k; end option k = step k + 2
        int sustainStep() const { return selected().sustain? selected().sustain->option() : 0; }
        int endStep() const { return selected().end? selected().end->option() + 2 : 0; }
        // the number a player reads: wave1 1..8, wave2 0 (Off)..8
        int waveNumber(int wave) const {
            const Parameter* p = (wave == 1)? wave1 : wave2;
            return p? p->option() + (wave == 1? 1 : 0) : 0;
        }
    };

    Parameter* find(ParameterSet& parameters, const std::string& id){
        Parameter* p = parameters.byId(id);
        if(p == nullptr && missing_.empty()){
            missing_ = id;
        }
        return p;
    }

    bool validRow(int r) const { return r >= 1 && r <= static_cast<int>(rows_.size()); }

    void selectKeyFollow(const Row& row){
        Parameter* const current = panel_.target();
        if(current == row.keyFollowDcw){
            panel_.select(row.keyFollowDca);
        }else if(current == row.keyFollowDca){
            panel_.select(row.keyFollowDcw);
        }else{
            panel_.select(row.eg == 2? row.keyFollowDca : row.keyFollowDcw);
        }
    }

    PanelLogic& panel_;
    std::vector<Row> rows_;
    std::string missing_;
};

// LINE SELECT: button "line" steps Line 1 -> Line 2 -> 1+1' -> 1+2'.
// LEDs show the lines that sound: "line.1", "line.2", "line.1'", "line.2'"
// (1+1' lights 1 and 1', 1+2' lights 1 and 2').
class PdLineSelect : public PanelLogic::Module {
public:
    PdLineSelect(PanelLogic& panel, ParameterSet& parameters)
        : panel_(panel), param_(parameters.byId("line_select")){}

    std::string unavailable() const override {
        return param_? std::string()
             : "line select: the instrument has no parameter 'line_select'";
    }

    PanelLogic::ButtonFn button(const std::string& role) override {
        if(role != "line"){
            return {};
        }
        return [this](bool pressed){ if(pressed) panel_.step(param_); };
    }

    PanelLogic::LedFn led(const std::string& role) override {
        // bit per LED, indexed by the line select option
        static const char* const names[4] = {"line.1", "line.2", "line.1'", "line.2'"};
        static const int lit[4] = {0b0001, 0b0010, 0b0101, 0b1001};
        for(int i = 0; i < 4; i++){
            if(role == names[i]){
                return [this, i]{
                    if(param_ == nullptr){
                        return LedState::Off;
                    }
                    return (lit[std::clamp(param_->option(), 0, 3)] >> i & 1)
                         ? LedState::On : LedState::Off;
                };
            }
        }
        return {};
    }

private:
    PanelLogic& panel_;
    Parameter* param_;
};

// Tone (preset) selection, CZ-101 style: two banks of 16 tones — preset
// and user — reached with 8 tone buttons and a page switch.
//
//   buttons  preset, user   choose the bank the tone buttons address
//            page           tone buttons 1-8 <-> 9-16
//            tone1..tone8   load bank / page / number (Program Change)
//            compare        A/B: the original tone <-> the edited one
//                           (only while the tone is edited or compared)
//            initialize     discard every edit to the current tone
//   LEDs     preset, user, page    the bank / page the tone buttons address
//            tone1..tone8   the current tone's number: lit when it is in
//                           the addressed bank and page, blinking when it
//                           is not (bank or page pressed, no tone yet)
//            compare        lit while the tone is edited, blinking while
//                           the original is being compared
//
// The program numbers: preset N = N - 1, user N = the last 16 slots of the
// bank. Whenever the current tone changes (here, MIDI Program Change, the
// preset-next button), the bank and page selection follow it.
class PdToneSelect : public PanelLogic::Module {
public:
    static constexpr int kTonesPerBank = 16;
    static constexpr int kButtons = 8;

    struct Host {
        std::function<void(int program)> programChange;
        std::function<void()> toggleCompare;
        std::function<void()> initialize;
    };

    PdToneSelect(PresetBank* bank, Host host) : bank_(bank), host_(std::move(host)){
        if(bank_ != nullptr && bank_->count() >= 2 * kTonesPerBank){
            userBase_ = bank_->count() - kTonesPerBank;
        }
    }

    std::string unavailable() const override {
        return (userBase_ >= 0)? std::string()
             : "tone select: the instrument needs at least 32 presets (16 preset + 16 user)";
    }

    PanelLogic::ButtonFn button(const std::string& role) override {
        int n = 0;
        auto onPress = [](std::function<void()> fn) -> PanelLogic::ButtonFn {
            return [fn](bool pressed){ if(pressed) fn(); };
        };
        if(role == "preset") return onPress([this]{ user_ = false; });
        if(role == "user")   return onPress([this]{ user_ = true; });
        if(role == "page")   return onPress([this]{ page_ = !page_; });
        if(role == "compare"){
            return onPress([this]{
                if(available() && (bank_->comparing() || bank_->isEdited())){
                    host_.toggleCompare();
                }
            });
        }
        if(role == "initialize"){
            return onPress([this]{ if(available()) host_.initialize(); });
        }
        if(pdpanel::numbered(role, "tone", n) && n >= 1 && n <= kButtons){
            return onPress([this, n]{
                if(available()){
                    host_.programChange((user_? userBase_ : 0) + (page_? kButtons : 0) + n - 1);
                }
            });
        }
        return {};
    }

    PanelLogic::LedFn led(const std::string& role) override {
        int n = 0;
        auto on = [](bool lit){ return lit? LedState::On : LedState::Off; };
        if(role == "preset") return [this, on]{ return on(available() && !user_); };
        if(role == "user")   return [this, on]{ return on(available() && user_); };
        if(role == "page")   return [this, on]{ return on(available() && page_); };
        if(role == "compare"){
            return [this]{
                if(!available()) return LedState::Off;
                if(bank_->comparing()) return LedState::Blink;
                return bank_->isEdited()? LedState::On : LedState::Off;
            };
        }
        if(pdpanel::numbered(role, "tone", n) && n >= 1 && n <= kButtons){
            return [this, n]{
                Location loc;
                if(!available() || !locate(bank_->current(), loc) || loc.number != n){
                    return LedState::Off;
                }
                return (loc.user == user_ && loc.page == page_)? LedState::On : LedState::Blink;
            };
        }
        return {};
    }

    void tick() override {
        if(!available()){
            return;
        }
        const int current = bank_->current();
        if(current != lastSeen_){
            lastSeen_ = current;
            Location loc;
            if(locate(current, loc)){
                user_ = loc.user;
                page_ = loc.page;
            }
        }
    }

private:
    struct Location {
        bool user = false;
        bool page = false;
        int number = 0;  // 1..8 on the page
    };

    bool available() const { return userBase_ >= 0; }

    bool locate(int program, Location& loc) const {
        int index = -1;
        if(program >= 0 && program < kTonesPerBank){
            loc.user = false;
            index = program;
        }else if(program >= userBase_ && program < userBase_ + kTonesPerBank){
            loc.user = true;
            index = program - userBase_;
        }
        if(index < 0){
            return false;
        }
        loc.page = index >= kButtons;
        loc.number = index % kButtons + 1;
        return true;
    }

    PresetBank* bank_;
    Host host_;
    int userBase_ = -1;
    int lastSeen_ = -1;
    bool user_ = false;
    bool page_ = false;
};

// DETUNE / VIBRATO: three encoders shared between the detune of the
// primed line and the vibrato, plus the vibrato switches.
//
//   button   detune-vib     which one the encoders edit   LEDs: detune, vib
//            vib.on         vibrato on/off                LED:  vib.on
//            vib.wave       tri -> up saw -> down saw -> square
//                           LEDs: vib.tri, vib.upsaw, vib.downsaw, vib.square
//   encoders dv.1  detune octave / vibrato delay
//            dv.2  detune note   / vibrato depth
//            dv.3  detune fine   / vibrato rate
//
// pd has no vibrato yet: the vibrato parameter ids below are the ones a
// future pd is expected to register; until then the vibrato side is idle
// (the detune side works).
class PdDetuneVibrato : public PanelLogic::Module {
public:
    struct VibratoIds {
        std::string enable = "vibrato";
        std::string wave = "vibrato_wave";
        std::string delay = "vibrato_delay";
        std::string depth = "vibrato_depth";
        std::string rate = "vibrato_rate";
    };

    PdDetuneVibrato(PanelLogic& panel, ParameterSet& parameters)
        : PdDetuneVibrato(panel, parameters, VibratoIds()){}

    PdDetuneVibrato(PanelLogic& panel, ParameterSet& parameters, const VibratoIds& ids)
        : panel_(panel){
        detune_ = {parameters.byId("detune_octave"), parameters.byId("detune_note"),
                   parameters.byId("detune_fine")};
        vibrato_ = {parameters.byId(ids.delay), parameters.byId(ids.depth),
                    parameters.byId(ids.rate)};
        enable_ = parameters.byId(ids.enable);
        wave_ = parameters.byId(ids.wave);
        if(enable_ == nullptr){
            missing_ = ids.enable;
        }
    }

    std::string unavailable() const override {
        return missing_.empty()? std::string()
             : "vibrato: the instrument has no parameter '" + missing_
               + "' (pd has no vibrato yet); the vib controls do nothing";
    }

    PanelLogic::ButtonFn button(const std::string& role) override {
        auto onPress = [](std::function<void()> fn) -> PanelLogic::ButtonFn {
            return [fn](bool pressed){ if(pressed) fn(); };
        };
        if(role == "detune-vib") return onPress([this]{ vibrato_mode_ = !vibrato_mode_; });
        if(role == "vib.on")     return onPress([this]{ panel_.step(enable_); });
        if(role == "vib.wave")   return onPress([this]{ panel_.step(wave_); });
        return {};
    }

    PanelLogic::EncoderFn encoder(const std::string& role) override {
        int n = 0;
        if(!pdpanel::numbered(role, "dv.", n) || n < 1 || n > 3){
            return {};
        }
        const size_t i = static_cast<size_t>(n - 1);
        return [this, i](int delta){
            panel_.nudge(vibrato_mode_? vibrato_[i] : detune_[i], delta);
        };
    }

    PanelLogic::LedFn led(const std::string& role) override {
        auto on = [](bool lit){ return lit? LedState::On : LedState::Off; };
        if(role == "detune") return [this, on]{ return on(!vibrato_mode_); };
        if(role == "vib")    return [this, on]{ return on(vibrato_mode_); };
        if(role == "vib.on"){
            return [this, on]{ return on(enable_ != nullptr && enable_->getNormalized() >= 0.5f); };
        }
        static const char* const waves[4] = {"vib.tri", "vib.upsaw", "vib.downsaw", "vib.square"};
        for(int i = 0; i < 4; i++){
            if(role == waves[i]){
                return [this, i, on]{ return on(wave_ != nullptr && wave_->option() == i); };
            }
        }
        return {};
    }

    bool vibratoMode() const { return vibrato_mode_; }

private:
    PanelLogic& panel_;
    std::array<Parameter*, 3> detune_{};
    std::array<Parameter*, 3> vibrato_{};
    Parameter* enable_ = nullptr;
    Parameter* wave_ = nullptr;
    std::string missing_;
    bool vibrato_mode_ = false;
};

}  // namespace rtsynth
