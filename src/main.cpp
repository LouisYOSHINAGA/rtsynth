// Standalone entry point: parses CLI options, wires a SineSynthProcessor
// into a StandaloneHost, then idles until SIGINT/SIGTERM. Designed to run
// headless (no interactive prompts) so it can be started from systemd on a
// hardware synth. This file is the only place that decides *which*
// Processor is built — swap the instrument here.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "host/StandaloneHost.hpp"
#include "synth/SineSynthProcessor.hpp"

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int){
    g_running.store(false);
}

void printUsage(const char* argv0){
    std::cout <<
        "Usage: " << argv0 << " [options]\n"
        "  -l, --list           list audio devices and MIDI ports, then exit\n"
        "  -d, --device <id>    audio output device id (default: system default)\n"
        "  -m, --midi <index>   MIDI input port index (default: auto)\n"
        "  -r, --rate <hz>      sample rate (default: 44100)\n"
        "  -b, --buffer <n>     buffer size in frames (default: 256)\n"
        "  -g, --gain <0..1>    master gain (default: 0.2)\n"
        "  -h, --help           show this help\n";
}

void listDevices(){
    rtsynth::RtAudioOutput audio;
    std::cout << "=== Audio Output Devices ===" << std::endl;
    for(const auto& device : audio.listOutputDevices()){
        std::cout << "  [" << device.id << "] " << device.name
                  << " (" << device.outputChannels << " ch)"
                  << (device.isDefault? " [default]" : "") << std::endl;
    }

    rtsynth::RtMidiInput midi;
    std::cout << "=== MIDI Input Ports ===" << std::endl;
    const auto ports = midi.listPorts();
    if(ports.empty()){
        std::cout << "  (none)" << std::endl;
    }
    for(size_t i = 0; i < ports.size(); i++){
        std::cout << "  [" << i << "] " << ports[i] << std::endl;
    }
}

}  // namespace

int main(int argc, char* argv[]){
    rtsynth::StandaloneHost::Options options;
    float gain = -1.0f;

    try{
    for(int i = 1; i < argc; i++){
        const std::string arg = argv[i];
        auto nextArg = [&]() -> const char* {
            return (i + 1 < argc)? argv[++i] : nullptr;
        };
        if(arg == "-h" || arg == "--help"){
            printUsage(argv[0]);
            return 0;
        }else if(arg == "-l" || arg == "--list"){
            listDevices();
            return 0;
        }else if(arg == "-d" || arg == "--device"){
            if(const char* v = nextArg()) options.audioDeviceId = std::stoul(v);
        }else if(arg == "-m" || arg == "--midi"){
            if(const char* v = nextArg()) options.midiPortIndex = std::stoi(v);
        }else if(arg == "-r" || arg == "--rate"){
            if(const char* v = nextArg()) options.sampleRate = std::stoul(v);
        }else if(arg == "-b" || arg == "--buffer"){
            if(const char* v = nextArg()) options.bufferFrames = std::stoul(v);
        }else if(arg == "-g" || arg == "--gain"){
            if(const char* v = nextArg()) gain = std::stof(v);
        }else{
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    }catch(const std::exception&){
        std::cerr << "Invalid option value." << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    rtsynth::SineSynthProcessor synth;
    if(gain >= 0.0f){
        synth.parameters().byId("gain")->set(gain);
    }

    rtsynth::StandaloneHost host(synth);
    if(!host.start(options)){
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout << "Running. Press Ctrl+C to quit." << std::endl;

    uint64_t lastXruns = 0;
    uint64_t lastDrops = 0;
    while(g_running.load()){
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // report problems from the main thread, never from RT threads
        const uint64_t xruns = host.audio().xrunCount();
        if(xruns != lastXruns){
            std::cerr << "[warning] audio under/overflow (total: " << xruns << ")" << std::endl;
            lastXruns = xruns;
        }
        const uint64_t drops = host.midi().droppedCount() + host.midiOverflowCount();
        if(drops != lastDrops){
            std::cerr << "[warning] MIDI events dropped (total: " << drops << ")" << std::endl;
            lastDrops = drops;
        }
    }

    std::cout << "\nShutting down." << std::endl;
    host.stop();
    return 0;
}
