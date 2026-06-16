#pragma once

#include <cmath>
#include <array>
#include <RtAudio.h>
#include <boost/lockfree/spsc_queue.hpp>


constexpr double SR = 44100;
constexpr uint8_t N_NOTES = 128;
constexpr uint8_t N_VOICES = 16;
constexpr double OUT_AMP = 0.1;
constexpr double A4FREQ = 440.0;
constexpr double A4NOTE = 69;
constexpr double TWO_PI = 2 * M_PI;


inline double note_to_freq(int note){
    return A4FREQ * std::pow(2.0, (static_cast<double>(note) - A4NOTE)/12.0);
}


struct NoteEvent{
    uint32_t note;
    bool is_note_on;
};


struct VoiceState{
    uint32_t note;
    double freq;
    uint64_t order = 0;
    double phase = 0;
};


struct SynthState{
    boost::lockfree::spsc_queue<
        NoteEvent, boost::lockfree::capacity<N_NOTES>
    > voice_queue;
    std::array<VoiceState, N_VOICES> voices = {};
    uint64_t voice_counter = 0;
};


class PrettySynth{
public:
    PrettySynth(SynthState& state): state_(state){}
    ~PrettySynth(){}
    void render(double* output, unsigned int nFrames);
private:
    SynthState& state_;
};


class AudioEngine{
public:
    AudioEngine(): audio_(RtAudio::LINUX_ALSA), synth_(state_){}
    ~AudioEngine(){ stop(); }

    bool start();
    void stop();

    SynthState& get_state(){ return state_; }

private:
    RtAudio audio_;
    SynthState state_;
    PrettySynth synth_;
};