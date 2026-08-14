#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include "RtAudioOutput.hpp"

#if defined(__linux__)
#include <alsa/asoundlib.h>
#endif

namespace rtsynth {

namespace {

#if defined(__linux__)
void discardAlsaError(const char*, int, const char*, int, const char*, ...){}

// Whether ALSA can open the control device named "default".
//
// This is not a cosmetic detail on RtAudio 5.x: its ALSA backend counts
// devices as [ "default" if this succeeds ] + [ hardware devices ], but
// *opens* them as [ "default" at id 0 ] + [ hardware devices from id 1 ].
// The optional term is exactly this check, so when it fails the counting
// runs one short of the opening, and the last hardware device lands
// outside the range openStream() accepts. It then cannot be opened by any
// id at all — which is what an I2S DAC plus dtparam=audio=off produces.
bool alsaDefaultIsUsable(){
    // a failing probe is the answer we are asking for, not something to
    // print: ALSA would otherwise dump half a screen of config traces on
    // top of the explanation we are about to give
    snd_lib_error_set_handler(&discardAlsaError);
    snd_ctl_t* handle = nullptr;
    const bool usable = snd_ctl_open(&handle, "default", SND_CTL_NONBLOCK) >= 0;
    if(usable){
        snd_ctl_close(handle);
    }
    snd_lib_error_set_handler(nullptr);  // back to ALSA's own handler
    return usable;
}

// The sound cards with the id an asound.conf "card" entry needs. Worth
// printing next to that advice because the id is the one field `aplay -l`
// does not put in brackets, so it is the easy one to copy wrongly.
std::string alsaCardList(){
    std::string text;
    snd_lib_error_set_handler(&discardAlsaError);
    snd_ctl_card_info_t* info = nullptr;
    if(snd_ctl_card_info_malloc(&info) == 0){
        int card = -1;
        while(snd_card_next(&card) == 0 && card >= 0){
            char device[32];
            std::snprintf(device, sizeof(device), "hw:%d", card);
            snd_ctl_t* handle = nullptr;
            if(snd_ctl_open(&handle, device, SND_CTL_NONBLOCK) < 0){
                continue;
            }
            if(snd_ctl_card_info(handle, info) == 0){
                text += "  card " + std::to_string(card) + ":  id "
                      + snd_ctl_card_info_get_id(info) + "   ("
                      + snd_ctl_card_info_get_name(info) + ")\n";
            }
            snd_ctl_close(handle);
        }
        snd_ctl_card_info_free(info);
    }
    snd_lib_error_set_handler(nullptr);
    return text;
}
#endif

}  // namespace

RtAudio::Api RtAudioOutput::resolveApi(const std::string& apiName){
    if(!apiName.empty()){
        const RtAudio::Api api = RtAudio::getCompiledApiByName(apiName);
        if(api != RtAudio::UNSPECIFIED){
            return api;
        }
        std::cerr << "Audio API '" << apiName
                  << "' is unknown or not compiled in; falling back to auto selection."
                  << std::endl;
    }

#if defined(__linux__)
    // Auto selection: prefer direct ALSA over a sound server. RtAudio's own
    // UNSPECIFIED order (JACK -> Pulse -> ALSA) picks PulseAudio/PipeWire on
    // desktop systems, which buffers on the server side and adds latency.
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    if(std::find(apis.begin(), apis.end(), RtAudio::LINUX_ALSA) != apis.end()){
        RtAudio probe(RtAudio::LINUX_ALSA);
        probe.showWarnings(false);  // enumeration only; skipped devices are fine
        if(probe.getDeviceCount() > 0){
            return RtAudio::LINUX_ALSA;
        }
    }
#endif
    return RtAudio::UNSPECIFIED;
}

RtAudio& RtAudioOutput::rt(){
    if(audio_ == nullptr){
        audio_ = std::make_unique<RtAudio>(resolveApi(requestedApi_));
        audio_->showWarnings(verboseWarnings_);
    }
    return *audio_;
}

std::string RtAudioOutput::currentApiName(){
    return RtAudio::getApiDisplayName(rt().getCurrentApi());
}

std::vector<AudioDeviceDesc> RtAudioOutput::listOutputDevices(){
    std::vector<AudioDeviceDesc> devices;
#if RTSYNTH_RTAUDIO_6
    for(unsigned int id : rt().getDeviceIds()){
        const RtAudio::DeviceInfo info = rt().getDeviceInfo(id);
        if(info.outputChannels > 0){
            devices.push_back({id, info.name, info.outputChannels, info.isDefaultOutput});
        }
    }
#else
    for(unsigned int i = 0; i < rt().getDeviceCount(); i++){
        const RtAudio::DeviceInfo info = rt().getDeviceInfo(i);
        if(info.probed && info.outputChannels > 0){
            devices.push_back({i, info.name, info.outputChannels, info.isDefaultOutput});
        }
    }
#endif
    return devices;
}

unsigned int RtAudioOutput::defaultOutputDevice(){
    return rt().getDefaultOutputDevice();
}

bool RtAudioOutput::tryOpenDevice(unsigned int deviceId, unsigned int sampleRate,
                                  RtAudio::StreamOptions& options, std::string& error){
    RtAudio::StreamParameters outputParams;
    outputParams.deviceId = deviceId;
    outputParams.nChannels = channels_;
    outputParams.firstChannel = 0;

#if RTSYNTH_RTAUDIO_6
    if(rt().openStream(&outputParams, nullptr, RTAUDIO_FLOAT32, sampleRate,
                       &bufferFrames_, &rtCallback, this, &options) != RTAUDIO_NO_ERROR){
        error = rt().getErrorText();
        return false;
    }
#else
    try{
        rt().openStream(&outputParams, nullptr, RTAUDIO_FLOAT32, sampleRate,
                        &bufferFrames_, &rtCallback, this, &options);
    }catch(const RtAudioError& e){
        error = e.getMessage();
        return false;
    }
#endif
    return true;
}

bool RtAudioOutput::open(unsigned int deviceId, unsigned int sampleRate,
                         unsigned int bufferFrames, unsigned int channels,
                         RenderCallback callback){
    const std::vector<AudioDeviceDesc> devices = listOutputDevices();
    if(devices.empty()){
        std::cerr << "No audio output device found." << std::endl;
        reportDeviceHelp(devices);
        return false;
    }

    callback_ = std::move(callback);
    channels_ = channels;
    bufferFrames_ = bufferFrames;
    sampleRate_ = static_cast<double>(sampleRate);
    channelPointers_.resize(channels);
    openedDeviceName_.clear();

    RtAudio::StreamOptions options;
    // non-interleaved -> the callback receives planar channel data
    options.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_NONINTERLEAVED;
    options.priority = 70;  // used by RtAudio when SCHEDULE_REALTIME succeeds
    options.streamName = "rtsynth";

    // With an explicit -d there is one candidate and a failure is the
    // user's to fix. Without one, try every device the backend will let us
    // name, because on RtAudio 5.x's ALSA backend neither the default nor
    // the enumerated ids can be trusted on their own (see the note below).
    std::vector<unsigned int> candidates;
    auto addCandidate = [&candidates](unsigned int id){
        if(std::find(candidates.begin(), candidates.end(), id) == candidates.end()){
            candidates.push_back(id);  // keep the first-listed priority order
        }
    };
    if(deviceId != kUseDefaultDevice){
        candidates.push_back(deviceId);
    }else{
        addCandidate(defaultOutputDevice());
        for(const AudioDeviceDesc& device : devices){
            addCandidate(device.id);
        }
#if !RTSYNTH_RTAUDIO_6
        // RtApiAlsa numbers devices twice and the two can disagree by one
        // (see alsaDefaultIsUsable above), so a listed id is not always the
        // id that opens that device. Sweeping every id openStream() accepts
        // covers the shift where it is coverable; where it is not — the
        // device pushed past the end of the range — nothing here can help,
        // and reportDeviceHelp() says so rather than leaving the user to
        // guess. openStream() rejects anything >= getDeviceCount().
        for(unsigned int id = 0; id < rt().getDeviceCount(); id++){
            addCandidate(id);
        }
#endif
    }

    std::string firstError;
    for(size_t i = 0; i < candidates.size(); i++){
        std::string error;
        // openStream() writes the size the driver settled on, so each
        // attempt has to start from the requested value again
        bufferFrames_ = bufferFrames;
        if(!tryOpenDevice(candidates[i], sampleRate, options, error)){
            if(i == 0){
                firstError = error;
            }
            continue;
        }
        openedDeviceName_ = "device " + std::to_string(candidates[i]);
        for(const AudioDeviceDesc& device : devices){
            if(device.id == candidates[i]){
                openedDeviceName_ = device.name;
            }
        }
        if(i > 0){
            std::cerr << "[warning] the default audio device could not be opened ("
                      << firstError << ")\n"
                         "          started on device " << candidates[i]
                      << " instead — pass -d " << candidates[i]
                      << " to select it directly" << std::endl;
        }
        return true;
    }

    std::cerr << "Failed to open audio stream: " << firstError << std::endl;
    reportDeviceHelp(devices);
    return false;
}

// Printed when no device could be opened. Retrying -d values is the
// natural next move and, in the case diagnosed below, a waste of time —
// so name the cause instead of listing ids that cannot work.
void RtAudioOutput::reportDeviceHelp(const std::vector<AudioDeviceDesc>& devices){
    if(!devices.empty()){
        std::cerr << "Output devices RtAudio enumerated:" << std::endl;
        for(const AudioDeviceDesc& device : devices){
            std::cerr << "  [" << device.id << "] " << device.name << std::endl;
        }
    }

#if !RTSYNTH_RTAUDIO_6 && defined(__linux__)
    if(rt().getCurrentApi() == RtAudio::LINUX_ALSA && !alsaDefaultIsUsable()){
        std::cerr <<
            "\nALSA has no usable \"default\" device, and on this RtAudio version that\n"
            "is fatal rather than inconvenient: its ALSA backend counts devices\n"
            "without \"default\" but numbers them for opening as if it existed, so the\n"
            "last sound card falls outside the range it accepts. No -d value reaches\n"
            "it — the id --list shows for it is already one too low.\n"
            "\n"
            "Fix it by giving ALSA a \"default\" that points at your card, then run\n"
            "rtsynth with no -d at all. In /etc/asound.conf (or ~/.asoundrc for just\n"
            "this user):\n"
            "\n"
            "  pcm.!default { type hw  card sndrpihifiberry }\n"
            "  ctl.!default { type hw  card sndrpihifiberry }\n"
            "\n"
            "\"card\" takes the card *id*: the bracketed name in /proc/asound/cards,\n"
            "which in `aplay -l` is the word after \"card N:\" — NOT the name that\n"
            "follows it in brackets.\n";
        const std::string cards = alsaCardList();
        if(!cards.empty()){
            std::cerr << "\nThe cards on this machine:\n" << cards;
        }
        std::cerr << "\nVerify with: speaker-test -D default -c 2" << std::endl;
        return;
    }
#endif

    std::cerr << "\nTry another id with -d, or check that the device is not already"
                 " in use." << std::endl;
}

bool RtAudioOutput::start(){
#if RTSYNTH_RTAUDIO_6
    if(rt().startStream() != RTAUDIO_NO_ERROR){
        std::cerr << "Failed to start audio stream: " << rt().getErrorText() << std::endl;
        close();
        return false;
    }
#else
    try{
        rt().startStream();
    }catch(const RtAudioError& e){
        std::cerr << "Failed to start audio stream: " << e.getMessage() << std::endl;
        close();
        return false;
    }
#endif
    return true;
}

void RtAudioOutput::close(){
    if(audio_ == nullptr){
        return;
    }
#if RTSYNTH_RTAUDIO_6
    if(audio_->isStreamRunning()){
        audio_->stopStream();
    }
#else
    try{
        if(audio_->isStreamRunning()){
            audio_->stopStream();
        }
    }catch(const RtAudioError& e){
        std::cerr << "Error while stopping stream: " << e.getMessage() << std::endl;
    }
#endif
    if(audio_->isStreamOpen()){
        audio_->closeStream();
    }
}

bool RtAudioOutput::audioThreadIsRealtime() const {
    const int policy = audioThreadPolicy();
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

int RtAudioOutput::rtCallback(void* outputBuffer, void* /*inputBuffer*/, unsigned int nFrames,
                              double /*streamTime*/, RtAudioStreamStatus status, void* userData){
    auto* self = static_cast<RtAudioOutput*>(userData);

    if(self->threadPolicy_.load(std::memory_order_relaxed) < 0){
        // one-time, non-blocking: record the scheduling policy this thread
        // really runs under so the main thread can warn when the requested
        // realtime scheduling was silently denied
        int policy = SCHED_OTHER;
        sched_param param{};
        pthread_getschedparam(pthread_self(), &policy, &param);
        self->threadPolicy_.store(policy, std::memory_order_relaxed);
    }

    if(status != 0){
        // no logging here: this is the RT thread; the host polls xrunCount()
        self->xruns_.fetch_add(1, std::memory_order_relaxed);
    }

    float* out = static_cast<float*>(outputBuffer);
    for(unsigned int ch = 0; ch < self->channels_; ch++){
        self->channelPointers_[ch] = out + ch * nFrames;
    }

    AudioBufferView view(self->channelPointers_.data(),
                         static_cast<int>(self->channels_), static_cast<int>(nFrames));

    // measure how much of this block's deadline the render consumes
    const auto begin = std::chrono::steady_clock::now();
    if(self->callback_){
        self->callback_(view);
    }else{
        view.clear();
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    const double blockSeconds = static_cast<double>(nFrames) / self->sampleRate_;
    if(blockSeconds > 0.0){
        const double spent =
            std::chrono::duration<double>(elapsed).count();
        const float load = static_cast<float>(spent / blockSeconds);
        self->load_.store(load, std::memory_order_relaxed);
        if(load > self->peakLoad_.load(std::memory_order_relaxed)){
            self->peakLoad_.store(load, std::memory_order_relaxed);
        }
    }
    return 0;
}

}  // namespace rtsynth
