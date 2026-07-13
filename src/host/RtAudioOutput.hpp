#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <RtAudio.h>
#include "../core/AudioBuffer.hpp"

namespace rtsynth {

// True when building against RtAudio >= 6 (Raspberry Pi OS "trixie" and
// later). The two major versions differ in ways that matter here, and this
// wrapper is the only place in the codebase that knows about either:
//   - 5.x identifies devices by index 0..getDeviceCount()-1;
//     6.x by opaque IDs from getDeviceIds(), where 0 is invalid
//   - 5.x reports errors by throwing RtAudioError;
//     6.x returns RtAudioErrorType (the exception class is gone)
// (RTAUDIO_VERSION_MAJOR is only defined by the 6.x headers.)
#if defined(RTAUDIO_VERSION_MAJOR) && RTAUDIO_VERSION_MAJOR >= 6
#define RTSYNTH_RTAUDIO_6 1
#else
#define RTSYNTH_RTAUDIO_6 0
#endif

struct AudioDeviceDesc {
    unsigned int id = 0;
    std::string name;
    unsigned int outputChannels = 0;
    bool isDefault = false;
};

// Thin host-side wrapper around RtAudio: device listing, stream lifetime,
// and the RT callback. Audio is float32 / non-interleaved, so the callback
// can hand the processor a planar AudioBufferView without copying.
//
// API (backend) selection matters for latency on Linux: RtAudio's own
// auto-selection probes JACK -> PulseAudio -> ALSA and picks the first one
// with devices, so on a desktop system it lands on PulseAudio/PipeWire —
// whose RtAudio 5.x backend leaves the playback buffer size to the sound
// server (requested buffer sizes are not honored), adding tens of ms of
// latency. setApi("") therefore prefers *direct ALSA* when it has devices,
// matching hardware-synth needs; pass "pulse", "jack", ... to override.
class RtAudioOutput {
public:
    // called on the audio thread once per block; must be RT-safe
    using RenderCallback = std::function<void(AudioBufferView&)>;

    // "use the default device" sentinel: RtAudio 5.x uses plain indices
    // where 0 is a real device, so 0 cannot mean "default" there
    static constexpr unsigned int kUseDefaultDevice = ~0u;

    // Select the backend by RtAudio API name ("alsa", "pulse", "jack",
    // "core", ...); "" = auto (prefer direct ALSA on Linux, see above).
    // Effective only before the first device/stream call.
    void setApi(const std::string& apiName){ requestedApi_ = apiName; }
    std::string currentApiName();

    std::vector<AudioDeviceDesc> listOutputDevices();
    unsigned int defaultOutputDevice();

    // open() only opens the stream (the driver may adjust the buffer size —
    // check actualBufferFrames() afterwards); call start() to begin rendering.
    bool open(unsigned int deviceId, unsigned int sampleRate,
              unsigned int bufferFrames, unsigned int channels,
              RenderCallback callback);
    bool start();
    void close();

    unsigned int actualBufferFrames() const { return bufferFrames_; }
    unsigned int channels() const { return channels_; }
    uint64_t xrunCount() const { return xruns_.load(std::memory_order_relaxed); }

private:
    static int rtCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                          double streamTime, RtAudioStreamStatus status, void* userData);

    static RtAudio::Api resolveApi(const std::string& apiName);

    // the RtAudio instance is created on first use so setApi() can be
    // called after construction (e.g. from parsed CLI options)
    RtAudio& rt();

    std::unique_ptr<RtAudio> audio_;
    std::string requestedApi_;
    RenderCallback callback_;
    std::vector<float*> channelPointers_;
    unsigned int bufferFrames_ = 0;
    unsigned int channels_ = 2;
    std::atomic<uint64_t> xruns_{0};
};

}  // namespace rtsynth
