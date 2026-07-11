#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

namespace rtsynth {

// A single automatable parameter, mirroring the VST idea of a normalized
// [0, 1] value that maps onto a plain (real-world) range:
//
//   plain value:      what the DSP uses, e.g. attack = 0.005 s
//   normalized value: what a host / MIDI CC / UI knob sees, always [0, 1]
//
// The stored value is atomic so any control thread (MIDI, hardware UI,
// OSC, ...) can write while the audio thread reads — no locks needed, and
// therefore safe to touch from process().
//
// Typical usage inside a Processor:
//
//   // constructor: register once, keep the returned pointer
//   attack_ = parameters_.add("attack", "Attack", 0.001f, 5.0f, 0.005f, "s");
//   // process(): read the current value (RT-safe)
//   env.setAttack(attack_->get());
class Parameter {
public:
    Parameter(std::string id, std::string name, float min, float max,
              float defaultValue, std::string unit = "")
        : id_(std::move(id)), name_(std::move(name)), unit_(std::move(unit)),
          min_(min), max_(max), defaultValue_(defaultValue), value_(defaultValue){}

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& unit() const { return unit_; }
    float min() const { return min_; }
    float max() const { return max_; }
    float defaultValue() const { return defaultValue_; }

    // plain-value access (RT-safe)
    float get() const { return value_.load(std::memory_order_relaxed); }
    void set(float plain){
        value_.store(std::clamp(plain, min_, max_), std::memory_order_relaxed);
    }

    // normalized [0, 1] access (what a VST host / MIDI CC sees)
    float getNormalized() const {
        return (max_ > min_)? (get() - min_) / (max_ - min_) : 0.0f;
    }
    void setNormalized(float normalized){
        set(min_ + std::clamp(normalized, 0.0f, 1.0f) * (max_ - min_));
    }

private:
    std::string id_, name_, unit_;
    float min_, max_, defaultValue_;
    std::atomic<float> value_;
};

// Parameter registry owned by a Processor. All parameters are created up
// front in the Processor's constructor — before the audio stream starts —
// so the set is immutable while audio runs and pointers into it stay
// valid for the Processor's lifetime. byId() does a linear search and is
// meant for setup/control code; the audio thread should use pointers
// obtained from add() instead.
class ParameterSet {
public:
    Parameter* add(std::string id, std::string name, float min, float max,
                   float defaultValue, std::string unit = ""){
        parameters_.push_back(std::make_unique<Parameter>(
            std::move(id), std::move(name), min, max, defaultValue, std::move(unit)));
        return parameters_.back().get();
    }

    Parameter* byId(const std::string& id){
        for(auto& p : parameters_){
            if(p->id() == id) return p.get();
        }
        return nullptr;
    }

    size_t size() const { return parameters_.size(); }
    Parameter& operator[](size_t index){ return *parameters_[index]; }

    auto begin(){ return parameters_.begin(); }
    auto end(){ return parameters_.end(); }

private:
    std::vector<std::unique_ptr<Parameter>> parameters_;
};

}  // namespace rtsynth
