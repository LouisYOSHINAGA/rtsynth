#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Parameters.hpp"

namespace rtsynth {

// A bank of complete parameter snapshots ("presets" / VST "programs") over
// one ParameterSet, selected by MIDI Program Change.
//
// The model is the one hardware synths use, and it is what makes "edit any
// preset with CC" work without a second copy of the parameter plumbing:
//
//   - the ParameterSet always holds the *live* values; MIDI CC, pots,
//     encoders and --param keep writing it directly and know nothing about
//     presets, so the currently selected slot is edited in place
//   - select() first stores the live values back into the slot being left,
//     then loads the new one — so switching away and back returns to the
//     edited sound, not the factory one
//   - edits live in RAM only; nothing is written to disk (yet), so a
//     restart brings the factory bank back
//
// Slots are registered once during instrument construction, so select()
// only ever assigns into already-sized vectors and is safe to call from
// the audio thread (a Program Change arrives as a MIDI event, like a CC).
class PresetBank {
public:
    explicit PresetBank(ParameterSet& parameters) : parameters_(parameters){}

    // Setup only: snapshot the ParameterSet's current values as a new slot.
    // Returns the slot index, which is also its Program Change number.
    int add(std::string name){
        std::vector<float> values(parameters_.size());
        names_.push_back(std::move(name));
        values_.push_back(std::move(values));
        store(count() - 1);
        // A second, never-overwritten copy: values_ tracks the sound as it
        // is being edited, factory_ stays as the instrument registered it,
        // which is what revertCurrent() undoes back to.
        factory_.push_back(values_.back());
        return count() - 1;
    }

    int count() const { return static_cast<int>(values_.size()); }
    bool empty() const { return values_.empty(); }
    int current() const { return current_.load(std::memory_order_relaxed); }
    const std::string& name(int index) const { return names_[static_cast<size_t>(index)]; }
    const std::string& currentName() const { return name(current()); }

    // Program Change: keep the edits made to the slot being left, then make
    // `index` the live sound. Returns false (and changes nothing) for an
    // unknown slot or when it is already selected. RT-safe.
    bool select(int index){
        if(index < 0 || index >= count() || index == current()){
            return false;
        }
        store(current());
        load(index);
        current_.store(index, std::memory_order_relaxed);
        revision_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Make the current slot's stored values live without saving anything —
    // used once after registering the factory bank. RT-safe.
    void loadCurrent(){
        load(current());
        revision_.fetch_add(1, std::memory_order_relaxed);
    }

    // Undo every edit made to the current slot, back to the values the
    // instrument registered at startup — the "panel revert" of a hardware
    // synth. This is deliberately not loadCurrent(): select() saves the
    // live values into the slot on the way out, so by the time you have
    // switched away and back, the slot's own copy *is* the edited sound
    // and reloading it would restore nothing.
    //
    // Returns false for an empty bank. RT-safe: the copy is element-wise
    // between two vectors that were sized once at registration.
    bool revertCurrent(){
        if(empty()){
            return false;
        }
        const size_t index = static_cast<size_t>(current());
        for(size_t i = 0; i < values_[index].size(); i++){
            values_[index][i] = factory_[index][i];
        }
        load(current());
        revision_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Bumped whenever every parameter changes at once (a preset switch or
    // a revert). A display watching this reports one line instead of the
    // hundred-odd parameter changes the bulk load produces.
    uint32_t revision() const { return revision_.load(std::memory_order_relaxed); }

    // The slot `delta` steps away in ring order: stepping past either end
    // continues from the other one, so a pair of panel buttons can reach
    // every slot without a dead stop at each end. Returns the current slot
    // for an empty bank.
    int neighbour(int delta) const {
        if(empty()){
            return current();
        }
        const int slots = count();
        // delta may be any size; the double modulo keeps the result in
        // [0, slots) for negative values too
        return ((current() + delta) % slots + slots) % slots;
    }

private:
    void store(int index){
        std::vector<float>& values = values_[static_cast<size_t>(index)];
        for(size_t i = 0; i < parameters_.size(); i++){
            values[i] = parameters_[i].get();
        }
    }

    void load(int index){
        const std::vector<float>& values = values_[static_cast<size_t>(index)];
        for(size_t i = 0; i < parameters_.size(); i++){
            parameters_[i].set(values[i]);
        }
    }

    ParameterSet& parameters_;
    std::vector<std::string> names_;
    std::vector<std::vector<float>> values_;
    std::vector<std::vector<float>> factory_;
    std::atomic<int> current_{0};
    std::atomic<uint32_t> revision_{0};
};

}  // namespace rtsynth
