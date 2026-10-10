#pragma once

#include <atomic>
#include <cstdint>

#include "../core/SpscRingBuffer.hpp"

namespace rtsynth {

// One debounced edge of a momentary button.
struct ButtonEvent {
    int channel = 0;
    bool pressed = false;  // true on the edge that closes the switch
};

// A bank of momentary buttons (GPIO tact switches, switches behind an I/O
// expander, ...). Implementations read their hardware on their own thread
// and publish() debounced edges; the ControlLoop thread drains them with
// popEvent() and acts on them there.
//
// The queue is the point of this class. Button actions end up in
// single-producer queues (StandaloneHost::sendControlEvent, the panel's
// own state), so they must all be issued from ONE thread no matter how
// many button sources exist — the ControlLoop thread. Each source in turn
// is the single producer of its own queue here.
class ButtonInput {
public:
    virtual ~ButtonInput() = default;

    virtual const char* name() const = 0;
    virtual int numChannels() const = 0;

    // consumer side (the ControlLoop thread)
    bool popEvent(ButtonEvent& out){ return events_.pop(out); }

    // edges lost because nobody drained the queue (should stay 0)
    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

protected:
    // producer side (the implementation's hardware thread)
    void publish(int channel, bool pressed){
        if(!events_.push({channel, pressed})){
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    SpscRingBuffer<ButtonEvent, 128> events_;
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace rtsynth
