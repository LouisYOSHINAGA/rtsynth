#pragma once

#include <atomic>
#include <cstddef>

namespace rtsynth {

// Minimal lock-free single-producer / single-consumer ring buffer, used to
// hand MIDI events from the MIDI callback thread to the audio thread.
// (Equivalent to boost::lockfree::spsc_queue, inlined here so the core has
// zero dependencies.)
//
// Correctness rules:
//   - exactly ONE thread may call push() and ONE thread may call pop();
//     with more threads on either side this is not safe
//   - one slot is always kept empty to distinguish "full" from "empty",
//     so the usable capacity is Capacity - 1
//   - the release store on each index pairs with the acquire load on the
//     other thread, making the written element visible before the index
//     that publishes it
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // producer side; returns false (drops) when the buffer is full
    bool push(const T& item){
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (Capacity - 1);
        if(next == tail_.load(std::memory_order_acquire)){
            return false;
        }
        items_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // consumer side
    bool pop(T& out){
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if(tail == head_.load(std::memory_order_acquire)){
            return false;
        }
        out = items_[tail];
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

private:
    T items_[Capacity];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

}  // namespace rtsynth
