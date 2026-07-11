#include <iostream>
#include "RtAudioOutput.hpp"

namespace rtsynth {

RtAudioOutput::RtAudioOutput() : audio_(RtAudio::UNSPECIFIED){}

std::vector<AudioDeviceDesc> RtAudioOutput::listOutputDevices(){
    std::vector<AudioDeviceDesc> devices;
#if RTSYNTH_RTAUDIO_6
    for(unsigned int id : audio_.getDeviceIds()){
        const RtAudio::DeviceInfo info = audio_.getDeviceInfo(id);
        if(info.outputChannels > 0){
            devices.push_back({id, info.name, info.outputChannels, info.isDefaultOutput});
        }
    }
#else
    for(unsigned int i = 0; i < audio_.getDeviceCount(); i++){
        const RtAudio::DeviceInfo info = audio_.getDeviceInfo(i);
        if(info.probed && info.outputChannels > 0){
            devices.push_back({i, info.name, info.outputChannels, info.isDefaultOutput});
        }
    }
#endif
    return devices;
}

unsigned int RtAudioOutput::defaultOutputDevice(){
    return audio_.getDefaultOutputDevice();
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
    if(audio_.openStream(&outputParams, nullptr, RTAUDIO_FLOAT32, sampleRate,
                         &bufferFrames_, &rtCallback, this, &options) != RTAUDIO_NO_ERROR){
        std::cerr << "Failed to open audio stream: " << audio_.getErrorText() << std::endl;
        return false;
    }
#else
    try{
        audio_.openStream(&outputParams, nullptr, RTAUDIO_FLOAT32, sampleRate,
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
    if(audio_.startStream() != RTAUDIO_NO_ERROR){
        std::cerr << "Failed to start audio stream: " << audio_.getErrorText() << std::endl;
        close();
        return false;
    }
#else
    try{
        audio_.startStream();
    }catch(const RtAudioError& e){
        std::cerr << "Failed to start audio stream: " << e.getMessage() << std::endl;
        close();
        return false;
    }
#endif
    return true;
}

void RtAudioOutput::close(){
#if RTSYNTH_RTAUDIO_6
    if(audio_.isStreamRunning()){
        audio_.stopStream();
    }
#else
    try{
        if(audio_.isStreamRunning()){
            audio_.stopStream();
        }
    }catch(const RtAudioError& e){
        std::cerr << "Error while stopping stream: " << e.getMessage() << std::endl;
    }
#endif
    if(audio_.isStreamOpen()){
        audio_.closeStream();
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
