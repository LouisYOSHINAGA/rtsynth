#include <algorithm>
#include <iostream>
#include "RtAudioOutput.hpp"

namespace rtsynth {

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

bool RtAudioOutput::open(unsigned int deviceId, unsigned int sampleRate,
                         unsigned int bufferFrames, unsigned int channels,
                         RenderCallback callback){
    if(listOutputDevices().empty()){
        std::cerr << "No audio output device found." << std::endl;
        return false;
    }

    callback_ = std::move(callback);
    channels_ = channels;
    bufferFrames_ = bufferFrames;
    channelPointers_.resize(channels);

    RtAudio::StreamParameters outputParams;
    outputParams.deviceId = (deviceId != kUseDefaultDevice)? deviceId : defaultOutputDevice();
    outputParams.nChannels = channels;
    outputParams.firstChannel = 0;

    RtAudio::StreamOptions options;
    // non-interleaved -> the callback receives planar channel data
    options.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_NONINTERLEAVED;
    options.streamName = "rtsynth";

#if RTSYNTH_RTAUDIO_6
    if(rt().openStream(&outputParams, nullptr, RTAUDIO_FLOAT32, sampleRate,
                       &bufferFrames_, &rtCallback, this, &options) != RTAUDIO_NO_ERROR){
        std::cerr << "Failed to open audio stream: " << rt().getErrorText() << std::endl;
        return false;
    }
#else
    try{
        rt().openStream(&outputParams, nullptr, RTAUDIO_FLOAT32, sampleRate,
                        &bufferFrames_, &rtCallback, this, &options);
    }catch(const RtAudioError& e){
        std::cerr << "Failed to open audio stream: " << e.getMessage() << std::endl;
        return false;
    }
#endif

    return true;
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

int RtAudioOutput::rtCallback(void* outputBuffer, void* /*inputBuffer*/, unsigned int nFrames,
                              double /*streamTime*/, RtAudioStreamStatus status, void* userData){
    auto* self = static_cast<RtAudioOutput*>(userData);

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
    if(self->callback_){
        self->callback_(view);
    }else{
        view.clear();
    }
    return 0;
}

}  // namespace rtsynth
