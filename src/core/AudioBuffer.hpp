#pragma once

#include <cstring>

namespace rtsynth {

// Non-owning view over planar (per-channel) float buffers, similar to
// JUCE's AudioBuffer<float> / VST3's AudioBusBuffers. "Planar" means one
// contiguous array per channel (LLLL... RRRR...), the layout plugin APIs
// use, rather than interleaved LRLR... — the host requests the same layout
// from the audio driver so no conversion is needed in the RT path.
//
// The host owns the storage and constructs a fresh view for every
// process() call; a Processor must not keep the pointers beyond one call.
class AudioBufferView {
public:
    AudioBufferView(float* const* channels, int numChannels, int numFrames)
        : channels_(channels), numChannels_(numChannels), numFrames_(numFrames){}

    int numChannels() const { return numChannels_; }
    int numFrames() const { return numFrames_; }

    float* channel(int index){ return channels_[index]; }
    const float* channel(int index) const { return channels_[index]; }

    void clear(){
        for(int ch = 0; ch < numChannels_; ch++){
            std::memset(channels_[ch], 0, sizeof(float) * static_cast<size_t>(numFrames_));
        }
    }

private:
    float* const* channels_;
    int numChannels_;
    int numFrames_;
};

}  // namespace rtsynth
