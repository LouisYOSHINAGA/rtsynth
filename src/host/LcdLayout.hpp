#pragma once

#include <cstdio>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

#include "ParameterDisplay.hpp"

namespace rtsynth {

// What the LCD's two rows say, as text templates in a file (--lcd-layout
// <file>), so the wording can change without touching code. One statement
// per line, '#' starts a comment, "|" separates the top row from the
// bottom row:
//
//   param <id pattern> <top> | <bottom>   a parameter changed
//   preset  <top> | <bottom>              a preset was loaded
//   startup <top> | <bottom>              shown when the synth starts
//   stop    <top> | <bottom>              shown when it stops
//
// The id pattern may use * (any text) and ? (one character); when several
// param lines match, the LAST one wins — so a catch-all "param *" comes
// first and the special cases after it.
//
// Placeholders:
//   param    {name}   built-in name        ("L1 DCW Rate 1")
//            {value}  built-in value text  ("0.787", "Saw", "Step 3")
//            {id}     parameter id         ("line1_dcw_rate1")
//            {raw}    0.000 .. 1.000
//            {99}     0 .. 99
//            {1} {2}  the text each * or ? of the pattern matched
//   preset   {n} (slot number)  {preset} (its name)
//   startup  {synth}  {preset}
//
// Each row is trimmed; wrap it in "..." to keep leading/trailing spaces.
// A row longer than the display is cut at its width.
struct LcdLayout {
    struct Rows { std::string top, bottom; };
    struct ParamRule { std::string pattern; Rows rows; };

    std::vector<ParamRule> params{{"*", {"{name}", "{value}"}}};
    Rows preset{"PRESET {n}", "{preset}"};
    Rows startup{"{synth}", "{preset}"};
    Rows stop{"rtsynth", "stopped"};

    Rows renderParameter(const DisplayLine& line) const {
        const ParamRule* rule = nullptr;
        std::vector<std::string> captures;
        for(const ParamRule& candidate : params){
            std::vector<std::string> found;
            if(match(candidate.pattern, line.id, found)){
                rule = &candidate;
                captures = std::move(found);
            }
        }
        if(rule == nullptr){
            return {line.label.empty()? line.id : line.label, line.value};
        }
        char raw[16], ninetyNine[8];
        std::snprintf(raw, sizeof(raw), "%.3f", static_cast<double>(line.normalized));
        std::snprintf(ninetyNine, sizeof(ninetyNine), "%d",
                      static_cast<int>(line.normalized * 99.0f + 0.5f));
        std::vector<std::pair<std::string, std::string>> values{
            {"name", line.label.empty()? line.id : line.label},
            {"value", line.value}, {"id", line.id}, {"raw", raw}, {"99", ninetyNine}};
        for(size_t i = 0; i < captures.size(); i++){
            values.emplace_back(std::to_string(i + 1), captures[i]);
        }
        return {fill(rule->rows.top, values), fill(rule->rows.bottom, values)};
    }

    Rows renderPreset(int index, const std::string& name) const {
        const std::vector<std::pair<std::string, std::string>> values{
            {"n", std::to_string(index)}, {"preset", name}};
        return {fill(preset.top, values), fill(preset.bottom, values)};
    }

    Rows renderStartup(const std::string& synth, const std::string& presetName) const {
        const std::vector<std::pair<std::string, std::string>> values{
            {"synth", synth}, {"preset", presetName}};
        return {fill(startup.top, values), fill(startup.bottom, values)};
    }

    // "{name}" -> its value; an unknown {word} stays as written, so a typo
    // shows up on the display instead of vanishing
    static std::string fill(const std::string& text,
                            const std::vector<std::pair<std::string, std::string>>& values){
        std::string out;
        for(size_t i = 0; i < text.size(); ){
            const size_t close = (text[i] == '{')? text.find('}', i) : std::string::npos;
            if(close != std::string::npos){
                const std::string key = text.substr(i + 1, close - i - 1);
                bool known = false;
                for(const auto& [k, v] : values){
                    if(k == key){
                        out += v;
                        known = true;
                        break;
                    }
                }
                if(known){
                    i = close + 1;
                    continue;
                }
            }
            out += text[i++];
        }
        return out;
    }

    // glob match of the whole text; captures = what each * / ? matched
    static bool match(const std::string& pattern, const std::string& text,
                      std::vector<std::string>& captures){
        return matchFrom(pattern, 0, text, 0, captures);
    }

private:
    static bool matchFrom(const std::string& p, size_t pi, const std::string& t, size_t ti,
                          std::vector<std::string>& captures){
        if(pi == p.size()){
            return ti == t.size();
        }
        if(p[pi] == '*'){
            for(size_t end = t.size() + 1; end-- > ti; ){   // longest first
                captures.push_back(t.substr(ti, end - ti));
                if(matchFrom(p, pi + 1, t, end, captures)){
                    return true;
                }
                captures.pop_back();
            }
            return false;
        }
        if(ti == t.size()){
            return false;
        }
        if(p[pi] == '?'){
            captures.push_back(t.substr(ti, 1));
            if(matchFrom(p, pi + 1, t, ti + 1, captures)){
                return true;
            }
            captures.pop_back();
            return false;
        }
        return p[pi] == t[ti] && matchFrom(p, pi + 1, t, ti + 1, captures);
    }
};

namespace lcd_layout_detail {

inline std::string trimRow(std::string text){
    const size_t first = text.find_first_not_of(" \t\r");
    if(first == std::string::npos){
        return {};
    }
    text = text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    if(text.size() >= 2 && text.front() == '"' && text.back() == '"'){
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

}  // namespace lcd_layout_detail

// Returns false with "line N: ..." in error. Statements replace the
// built-in defaults, except param rules, which are added after the
// built-in catch-all "param * {name} | {value}".
inline bool parseLcdLayout(std::istream& in, LcdLayout& layout, std::string& error){
    using lcd_layout_detail::trimRow;
    std::string text;
    int lineNo = 0;
    while(std::getline(in, text)){
        lineNo++;
        const size_t hash = text.find('#');
        if(hash != std::string::npos){
            text.resize(hash);
        }
        const size_t keyStart = text.find_first_not_of(" \t\r");
        if(keyStart == std::string::npos){
            continue;
        }
        const size_t keyEnd = text.find_first_of(" \t", keyStart);
        const std::string key = text.substr(keyStart, keyEnd - keyStart);
        std::string rest = (keyEnd == std::string::npos)? "" : text.substr(keyEnd);
        auto fail = [&](const std::string& message){
            error = "line " + std::to_string(lineNo) + ": " + message;
            return false;
        };

        std::string pattern;
        if(key == "param"){
            const size_t start = rest.find_first_not_of(" \t");
            if(start == std::string::npos){
                return fail("param needs an id pattern");
            }
            const size_t end = rest.find_first_of(" \t", start);
            pattern = rest.substr(start, end - start);
            rest = (end == std::string::npos)? "" : rest.substr(end);
        }else if(key != "preset" && key != "startup" && key != "stop"){
            return fail("unknown statement '" + key + "' (param, preset, startup, stop)");
        }

        const size_t bar = rest.find('|');
        if(bar == std::string::npos){
            return fail("expected '<top row> | <bottom row>'");
        }
        const LcdLayout::Rows rows{trimRow(rest.substr(0, bar)), trimRow(rest.substr(bar + 1))};
        if(key == "param"){
            layout.params.push_back({pattern, rows});
        }else if(key == "preset"){
            layout.preset = rows;
        }else if(key == "startup"){
            layout.startup = rows;
        }else{
            layout.stop = rows;
        }
    }
    return true;
}

inline bool loadLcdLayout(const std::string& path, LcdLayout& layout, std::string& error){
    std::ifstream file(path);
    if(!file){
        error = "cannot open " + path;
        return false;
    }
    if(!parseLcdLayout(file, layout, error)){
        error = path + ", " + error;
        return false;
    }
    return true;
}

}  // namespace rtsynth
