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
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "host/ControlLoop.hpp"
#include "host/Mcp3008Input.hpp"
#include "host/StandaloneHost.hpp"
#include "synth/SineSynthProcessor.hpp"
#ifdef RTSYNTH_HAVE_PD
#include "synth/PdSynthProcessor.hpp"
#endif

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int){
    g_running.store(false);
}

void printUsage(const char* argv0){
    std::cout <<
        "Usage: " << argv0 << " [options]\n"
        "  -s, --synth <name>   instrument to run: sine, pd (default: sine)\n"
        "  -l, --list           list audio devices and MIDI ports, then exit\n"
        "  -a, --api <name>     audio API: alsa, pulse, jack, ...\n"
        "                       (default: direct ALSA when available — lowest latency)\n"
        "  -d, --device <id>    audio output device id (default: system default)\n"
        "  -m, --midi <index>   restrict MIDI input to one port index\n"
        "                       (default: connect to all ports, e.g. keyboard + CC box)\n"
        "  -r, --rate <hz>      sample rate (default: 44100)\n"
        "  -b, --buffer <n>     buffer size in frames (default: 256)\n"
        "  -g, --gain <0..1>    master gain (default: 0.2)\n"
        "  -p, --param <id=v>   set a synth parameter, repeatable\n"
        "                       (e.g. --param attack=0.001 --param release=0.1)\n"
        "  --adc <ch>=<id>      map an MCP3008 ADC channel to a parameter, repeatable\n"
        "                       (e.g. --adc 0=gain --adc 1=attack); needs SPI enabled\n"
        "  --adc-device <path>  SPI device of the ADC (default: /dev/spidev0.0)\n"
        "  -h, --help           show this help\n";
}

void listDevices(const std::string& apiName){
    rtsynth::RtAudioOutput audio;
    audio.setApi(apiName);
    std::cout << "Audio API: " << audio.currentApiName() << std::endl;
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
    bool listRequested = false;
    std::string synthName = "sine";
    std::vector<std::pair<std::string, float>> paramOverrides;
    std::vector<std::pair<int, std::string>> adcMappings;
    std::string adcDevice = "/dev/spidev0.0";

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
            listRequested = true;  // handled after parsing so --api applies
        }else if(arg == "-s" || arg == "--synth"){
            if(const char* v = nextArg()) synthName = v;
        }else if(arg == "-a" || arg == "--api"){
            if(const char* v = nextArg()) options.audioApiName = v;
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
        }else if(arg == "-p" || arg == "--param"){
            if(const char* v = nextArg()){
                const std::string assignment = v;
                const size_t eq = assignment.find('=');
                if(eq == std::string::npos){
                    std::cerr << "--param expects <id>=<value>, got: " << assignment << std::endl;
                    return 1;
                }
                paramOverrides.emplace_back(assignment.substr(0, eq),
                                            std::stof(assignment.substr(eq + 1)));
            }
        }else if(arg == "--adc"){
            if(const char* v = nextArg()){
                const std::string assignment = v;
                const size_t eq = assignment.find('=');
                if(eq == std::string::npos){
                    std::cerr << "--adc expects <channel>=<param id>, got: " << assignment << std::endl;
                    return 1;
                }
                adcMappings.emplace_back(std::stoi(assignment.substr(0, eq)),
                                         assignment.substr(eq + 1));
            }
        }else if(arg == "--adc-device"){
            if(const char* v = nextArg()) adcDevice = v;
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

    if(listRequested){
        listDevices(options.audioApiName);
        return 0;
    }

    std::unique_ptr<rtsynth::Processor> synth;
    if(synthName == "sine"){
        synth = std::make_unique<rtsynth::SineSynthProcessor>();
    }else if(synthName == "pd"){
#ifdef RTSYNTH_HAVE_PD
        synth = std::make_unique<rtsynth::PdSynthProcessor>();
#else
        std::cerr << "This build has no PD synth (external/pd submodule was missing).\n"
                     "Run: git submodule update --init && rebuild." << std::endl;
        return 1;
#endif
    }else{
        std::cerr << "Unknown synth: " << synthName << " (available: sine, pd)" << std::endl;
        return 1;
    }
    std::cout << "Instrument: " << synth->name() << std::endl;

    // -g targets the master output whatever the instrument calls it
    if(gain >= 0.0f){
        rtsynth::Parameter* master = synth->parameters().byId("gain");
        if(master == nullptr){
            master = synth->parameters().byId("volume");
        }
        if(master != nullptr){
            master->set(gain);
        }
    }

    auto findParameter = [&synth](const std::string& id) -> rtsynth::Parameter* {
        rtsynth::Parameter* parameter = synth->parameters().byId(id);
        if(parameter == nullptr){
            std::cerr << "Unknown parameter '" << id << "'. Available:" << std::endl;
            for(auto& p : synth->parameters()){
                std::cerr << "  " << p->id() << " [" << p->min() << ".." << p->max()
                          << "] (default " << p->defaultValue() << ")"
                          << (p->unit().empty()? "" : " ") << p->unit() << std::endl;
            }
        }
        return parameter;
    };

    for(const auto& [id, value] : paramOverrides){
        rtsynth::Parameter* parameter = findParameter(id);
        if(parameter == nullptr){
            return 1;
        }
        parameter->set(value);
    }

    rtsynth::StandaloneHost host(*synth);
    if(!host.start(options)){
        return 1;
    }

    // optional hardware knobs: MCP3008 channels polled into parameters
    rtsynth::Mcp3008Input adc;
    rtsynth::ControlLoop controlLoop(adc);
    if(!adcMappings.empty()){
        if(!adc.open(adcDevice)){
            return 1;
        }
        for(const auto& [channel, id] : adcMappings){
            rtsynth::Parameter* parameter = findParameter(id);
            if(parameter == nullptr){
                return 1;
            }
            controlLoop.addMapping(channel, parameter);
        }
        controlLoop.start();
        std::cout << "ADC control: " << adc.name() << " on " << adcDevice
                  << ", " << adcMappings.size() << " mapping(s)" << std::endl;
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
    controlLoop.stop();
    host.stop();
    return 0;
}
