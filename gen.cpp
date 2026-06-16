#include <iostream>
#include <algorithm>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <RtAudio.h>
#include "gen.hpp"


constexpr uint32_t BUFFER_FRAMES_DEFAULT = 256;


void PrettySynth::render(double* output, unsigned int nFrames){
    NoteEvent note_event;
    while(state_.voice_queue.pop(note_event)){
        if(note_event.is_note_on){
            uint8_t index = UINT8_MAX;
            uint64_t oldest_same_voice_order = UINT64_MAX;
            uint8_t oldest_same_voice_index = UINT8_MAX;
            uint64_t oldest_order = UINT64_MAX;
            uint8_t oldest_voice_index = UINT8_MAX;

            for(uint8_t i = 0; i < N_VOICES; i++){
                if(state_.voices[i].order == 0){  // unassigned voice
                    index = i;
                    break;
                }else{
                    if(state_.voices[i].note == note_event.note
                    && state_.voices[i].order < oldest_same_voice_order){  // same note
                        oldest_same_voice_order = state_.voices[i].order;
                        oldest_same_voice_index = i;
                    }
                    if(state_.voices[i].order < oldest_order){  // oldest note
                        oldest_order = state_.voices[i].order;
                        oldest_voice_index = i;
                    }
                }
            }

            if(index == UINT8_MAX){  // unassigned voice is not found
                if(oldest_same_voice_index != UINT8_MAX){  // overwrite same note
                    index = oldest_same_voice_index;
                }else{  // overwrite oldest note
                    index = oldest_voice_index;
                }
            }

            state_.voices[index] = {
                .note = note_event.note,
                .freq = note_to_freq(note_event.note),
                .order = ++state_.voice_counter,
                .phase = 0
            };
        }else{
            uint64_t newest_same_voice_order = 0;
            uint8_t newest_same_voice_index = UINT8_MAX;
            for(uint8_t i = 0; i < N_VOICES; i++){
                if(state_.voices[i].note == note_event.note
                && state_.voices[i].order > newest_same_voice_order){
                    newest_same_voice_order = state_.voices[i].order;
                    newest_same_voice_index = i;
                }
            }
            if(newest_same_voice_index != UINT8_MAX){
                state_.voices[newest_same_voice_index].order = 0;
            }
        }
    }

    memset(output, 0, 2*nFrames*sizeof(double));

    for(uint16_t i = 0; i < nFrames; i++){
        for(uint8_t j = 0; j < N_VOICES; j++){
            if(state_.voices[j].order == 0){
                continue;
            }

            output[2*i] += std::sin(state_.voices[j].phase);
            output[2*i+1] += std::sin(state_.voices[j].phase);

            state_.voices[j].phase += TWO_PI * state_.voices[j].freq / SR;
            if(state_.voices[j].phase >= TWO_PI){
                state_.voices[j].phase -= TWO_PI;
            }
        }
        output[2*i] = std::clamp(OUT_AMP * output[2*i], -1.0, 1.0);
        output[2*i+1] = std::clamp(OUT_AMP * output[2*i+1], -1.0, 1.0);
    }
}


unsigned int selectAudioDevice(RtAudio& audio){
    const unsigned int nDevices = audio.getDeviceCount();
    const unsigned int defaultOutput = audio.getDefaultOutputDevice();

    if(nDevices == 0){
        std::cerr << "Audio output device is not found." << std::endl;
        return 0;
    }

    // show available audio output devices
    std::cout << "=== Audio Output Devices ===" << std::endl;
    for(uint8_t i = 0; i < nDevices; i++){
        RtAudio::DeviceInfo info = audio.getDeviceInfo(i);
        if(info.outputChannels > 0){
            std::cout << "  [" << +i << "] " << info.name
                      << " (Number of Output Channels: " << info.outputChannels << ")"
                      << ((i == defaultOutput)? " [default]": "")
                      << std::endl;
        }
    }

    // select audio output device
    uint16_t id = 0;
    std::cout << "Input Device Number: ";
    std::cin >> id;
    if(id >= nDevices){
        id = defaultOutput;
    }

    return id;
}


int audio_callback(void* output, void* input, unsigned int nFrames, double streamTime,
                   RtAudioStreamStatus status, void* userData){
    if(status){
        std::cerr << "[Warning] underflowe/overflow is detected." << std::endl;
    }

    static_cast<PrettySynth*>(userData)->render(static_cast<double*>(output), nFrames);

    return 0;
}


bool AudioEngine::start(){
    if(audio_.getDeviceCount() == 0){
        std::cerr << "No audio device is found." << std::endl;
        return false;
    }

    RtAudio::StreamParameters outputParams;
    outputParams.deviceId = selectAudioDevice(audio_);
    outputParams.nChannels = 2;  // stereo
    outputParams.firstChannel = 0;

    unsigned int bufferFrames = BUFFER_FRAMES_DEFAULT;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_SCHEDULE_REALTIME;

    // open audio stream
    try{
        audio_.openStream(&outputParams, nullptr, RTAUDIO_FLOAT64,
                          static_cast<unsigned int>(SR), &bufferFrames,
                          &audio_callback, &synth_, &options);
    }catch(const RtAudioError& e) {
        std::cerr << "Failed to open audio stream. " << e.getMessage() << std::endl;
        return false;
    }

    // start audio stream
    try{
        audio_.startStream();
    }catch(const RtAudioError& e){
        std::cerr << "Failed to start audio stream." << e.getMessage() << std::endl;
        return false;
    }

    return true;
}


void AudioEngine::stop(){
    // stop stream
    try {
        if(audio_.isStreamRunning()){
            audio_.stopStream();
        }
    }catch(const RtAudioError& e){
        std::cerr << "An error occurred when the stream was stopped. " << e.getMessage() << std::endl;
    }

    // close stream
    if(audio_.isStreamOpen()){
        audio_.closeStream();
    }
}
