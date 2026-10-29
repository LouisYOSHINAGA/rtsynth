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
//            rowR.sus        sustain point: Off, 1, ..., end - 1, Off, ...
//                            (only the steps before the end point)
//            rowR.end        end point: 2..8, 2, ...; moving it onto or in
//                            front of the sustain point turns sustain Off
//            rowR.wave1      first waveform: 1..8, 1, ...
//            rowR.wave2      second waveform: Off, 1..8, Off, ...
//            rowR.wave1 + rowR.wave2 pressed together: key follow mode
//                            on/off. In key follow mode the same two
//                            buttons step the DCW (wave1) and DCA (wave2)
//                            key follow 0..9, 0, ...
//            rowR.keyfollow  (optional) the same toggle on its own button
//   encoders rowR.1 .. rowR.8   rate / level K of the selected DCx (no
//                               level 8 in pd: idle on the level page)
//   LEDs     rowR.dco/dcw/dca, rowR.rate/level   the selection
//            rowR.step1..8   the sustain and the end step of the selected
//                            DCx: 2 lit = lower is sustain, higher is end;
//                            1 lit = end (sustain Off, or at/after the end)
//            rowR.wave1.1..9 one LED per value, the current one lit:
//            rowR.wave2.1..9 the waveform (1..8; wave2 Off = all dark), or
//                            in key follow mode the DCW / DCA key follow
//                            (0 = all dark, 1..9)
//            rowR.kf         lit in key follow mode
//
// The waveform buttons act when they are released, not pressed: only
// then is it known that the press was not half of a two-button chord.
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
        if(name == "sus")   return onPress([this, row]{ stepSustain(*row); });
        if(name == "end")   return onPress([this, row]{ stepEnd(*row); });
        if(name == "wave1") return [this, row](bool pressed){ waveButton(*row, 0, pressed); };
        if(name == "wave2") return [this, row](bool pressed){ waveButton(*row, 1, pressed); };
        if(name == "keyfollow") return onPress([this, row]{ toggleKeyFollow(*row); });
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
            return [row, n, on]{
                return on(n == row->endStep() || n == row->shownSustainStep());
            };
        }
        for(int wave = 1; wave <= 2; wave++){
            const std::string prefix = "wave" + std::to_string(wave) + ".";
            if(pdpanel::numbered(name, prefix, n) && n >= 1 && n <= 9){
                return [row, wave, n, on]{ return on(row->shownValue(wave) == n); };
            }
        }
        if(name == "kf") return [row, on]{ return on(row->keyFollowMode); };
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
        bool keyFollowMode = false;
        std::array<bool, 2> waveHeld{};   // wave1 / wave2 button down
        bool chord = false;               // both went down this time
        int eg = 0;           // 0 DCO, 1 DCW, 2 DCA
        bool level = false;   // false = rate page

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
        // The sustain point as the step LEDs show it. On the CZ a sustain
        // point never lies beyond the end point, so the panel reads two lit
        // LEDs as "lower = sustain, higher = end": a sustain at or after the
        // end is shown as no sustain at all (one LED = the end point).
        int shownSustainStep() const {
            const int sustain = sustainStep();
            return (sustain > 0 && sustain < endStep())? sustain : 0;
        }
        // the number a player reads: wave1 1..8, wave2 0 (Off)..8
        // what the wave LEDs show: the waveform number, or in key follow
        // mode the DCW (wave1) / DCA (wave2) key follow
        int shownValue(int wave) const {
            if(keyFollowMode){
                const Parameter* p = (wave == 1)? keyFollowDcw : keyFollowDca;
                return p? p->option() : 0;
            }
            return waveNumber(wave);
        }
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

    // The CZ keeps a sustain point before the end point, so the panel only
    // ever offers those: SUS cycles Off, 1 .. end - 1 (a sustain left at or
    // after the end by a preset or a CC counts as Off, as pd plays it), and
    // an END that lands on or in front of the sustain point drops it.
    // pd option encodings: sustain option k = step k (0 = Off); end option
    // k = step k + 2.
    void stepSustain(Row& row){
        Parameter* sustain = row.selected().sustain;
        if(sustain == nullptr){
            return;
        }
        const int next = row.shownSustainStep() + 1;
        panel_.setOption(sustain, (next < row.endStep())? next : 0);
    }

    void stepEnd(Row& row){
        EgParams& eg = row.selected();
        panel_.step(eg.end);
        if(eg.sustain != nullptr && row.sustainStep() >= row.endStep()){
            eg.sustain->setOption(0);
        }
    }

    // wave1 (i = 0) / wave2 (i = 1): one alone steps on release, both
    // together toggle key follow mode and do nothing else
    void waveButton(Row& row, size_t i, bool pressed){
        if(pressed){
            row.waveHeld[i] = true;
            if(row.waveHeld[1 - i]){
                row.chord = true;
                toggleKeyFollow(row);
            }
            return;
        }
        if(!row.waveHeld[i]){
            return;
        }
        row.waveHeld[i] = false;
        if(!row.chord){
            if(row.keyFollowMode){
                panel_.step(i == 0? row.keyFollowDcw : row.keyFollowDca);
            }else{
                panel_.step(i == 0? row.wave1 : row.wave2);
            }
        }
        if(!row.waveHeld[1 - i]){
            row.chord = false;   // both up: the next press starts afresh
        }
    }

    void toggleKeyFollow(Row& row){
        row.keyFollowMode = !row.keyFollowMode;
        if(row.keyFollowMode){
            panel_.select(row.keyFollowDcw);   // the LCD names what is now shown
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
// and user — reached with 8 tone buttons and a page switch. Every button
// switches the tone at once, so the LEDs always describe the tone that
// is sounding:
//
//   buttons  preset         the same number in the other bank, so each
//                           press alternates: preset 3 -> user 3 ->
//                           preset 3 ...
//            page           the same button on the other page
//                           (3 -> 11, 11 -> 3)
//            tone1..tone8   that number on the current bank and page
//            compare        A/B: the original tone <-> the edited one
//                           (only while the tone is edited or compared)
//            initialize     discard every edit to the current tone
//   LEDs     preset, user   which bank the current tone is in
//            page, tone1..tone8                 the current tone
//            compare        lit while the tone is edited, blinking while
//                           the original is being compared
//
// The program numbers: preset N = N - 1, user N = the last 16 slots of the
// bank. A Program Change from anywhere else (MIDI, preset-next) shows up
// the same way.
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
        if(role == "preset") return onPress([this]{ go([](Location& l){ l.user = !l.user; }); });
        if(role == "page")   return onPress([this]{ go([](Location& l){ l.page = !l.page; }); });
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
            return onPress([this, n]{ go([n](Location& l){ l.number = n; }); });
        }
        return {};
    }

    PanelLogic::LedFn led(const std::string& role) override {
        int n = 0;
        auto on = [](bool lit){ return lit? LedState::On : LedState::Off; };
        auto at = [this](auto test){
            return [this, test]{
                Location loc;
                return (available() && locate(shown(), loc) && test(loc))? LedState::On
                                                                        : LedState::Off;
            };
        };
        if(role == "preset") return at([](const Location& l){ return !l.user; });
        if(role == "user")   return at([](const Location& l){ return l.user; });
        if(role == "page")   return at([](const Location& l){ return l.page; });
        if(role == "compare"){
            return [this, on]{
                if(!available()) return LedState::Off;
                if(bank_->comparing()) return LedState::Blink;
                return on(bank_->isEdited());
            };
        }
        if(pdpanel::numbered(role, "tone", n) && n >= 1 && n <= kButtons){
            return at([n](const Location& l){ return l.number == n; });
        }
        return {};
    }

    void tick() override {
        // A requested switch lands on the audio thread a block or two
        // later; until then the panel acts on the request, so pressing
        // page twice quickly goes there and back instead of twice there.
        if(pending_ >= 0 && (bank_->current() == pending_ || --pendingTicks_ <= 0)){
            pending_ = -1;
        }
    }

private:
    struct Location {
        bool user = false;
        bool page = false;
        int number = 1;  // 1..8 on the page
    };

    static constexpr int kPendingTicks = 50;   // 0.5 s at 100 Hz

    bool available() const { return userBase_ >= 0; }

    // the tone the panel stands on: the requested one, until it lands
    int shown() const { return (pending_ >= 0)? pending_ : bank_->current(); }

    template <typename Change>
    void go(Change change){
        if(!available()){
            return;
        }
        Location loc;
        if(!locate(shown(), loc)){
            loc = Location{};   // a slot outside both banks: start from preset 1
        }
        change(loc);
        const int program = (loc.user? userBase_ : 0) + (loc.page? kButtons : 0) + loc.number - 1;
        if(program != shown()){
            pending_ = program;
            pendingTicks_ = kPendingTicks;
            host_.programChange(program);
        }
    }

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
    int pending_ = -1;
    int pendingTicks_ = 0;
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
