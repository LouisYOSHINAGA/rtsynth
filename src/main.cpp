// Standalone entry point: parses CLI options, wires the chosen Processor
// into a StandaloneHost together with the optional hardware controls
// (MCP3008 pots, GPIO rotary encoders), then idles until SIGINT/SIGTERM.
// Designed to run headless (no interactive prompts) so it can be started
// from systemd on a hardware synth. This file is the only place that
// decides *which* Processor is built — swap the instrument here.

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
#include "host/GpioEncoderInput.hpp"
#include "host/Mcp3008Input.hpp"
#include "host/ParameterWatcher.hpp"
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
        "  --enc <A>,<B>=<id>   map a rotary encoder on GPIO pins A/B to a parameter,\n"
        "                       repeatable (e.g. --enc 17,27=line1_dcw_level1)\n"
        "  --enc-chip <path>    GPIO chip of the encoders (default: /dev/gpiochip0)\n"
        "  --enc-step <size>    normalized change per encoder detent (default: 0.01)\n"
        "  -v, --verbose        print received MIDI events and parameter changes,\n"
        "                       and show audio backend warnings\n"
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

// --- verbose (-v) console output; an LCD would consume the same sources ------

void printMidiEvent(const std::string& portName, const rtsynth::MidiEvent& e){
    std::cout << "[midi] ";
    switch(e.type){
        case rtsynth::MidiEvent::Type::NoteOn:
            std::cout << "note on  ch " << +e.channel << "  note " << +e.data1
                      << "  vel " << +e.data2;
            break;
        case rtsynth::MidiEvent::Type::NoteOff:
            std::cout << "note off ch " << +e.channel << "  note " << +e.data1;
            break;
        case rtsynth::MidiEvent::Type::ControlChange:
            std::cout << "cc       ch " << +e.channel << "  cc " << +e.data1
                      << "  val " << +e.data2;
            break;
        case rtsynth::MidiEvent::Type::PitchBend:
            std::cout << "bend     ch " << +e.channel << "  value " << e.pitchBend14;
            break;
    }
    std::cout << "  (" << portName << ")" << std::endl;
}

void printParameterChange(rtsynth::Parameter& p){
    std::cout << "[param] " << p.id() << " = " << p.get()
              << (p.unit().empty()? "" : " ") << p.unit() << std::endl;
}

// --- option containers --------------------------------------------------------

struct EncoderMapping {
    unsigned int pinA;
    unsigned int pinB;
    std::string paramId;
};

struct CliOptions {
    rtsynth::StandaloneHost::Options host;
    std::string synthName = "sine";
    float gain = -1.0f;
    bool listRequested = false;
    std::vector<std::pair<std::string, float>> paramOverrides;
    std::vector<std::pair<int, std::string>> adcMappings;
    std::string adcDevice = "/dev/spidev0.0";
    std::vector<EncoderMapping> encoderMappings;
    std::string encoderChip = "/dev/gpiochip0";
    float encoderStep = 0.01f;
};

// Returns false (after printing a message) when the arguments are invalid;
// exitRequested is set when a flag like --help fully handles the run.
bool parseArguments(int argc, char* argv[], CliOptions& cli, bool& exitRequested){
    exitRequested = false;
    try{
        for(int i = 1; i < argc; i++){
            const std::string arg = argv[i];
            auto nextArg = [&]() -> const char* {
                return (i + 1 < argc)? argv[++i] : nullptr;
            };
            auto splitAssignment = [](const std::string& text, const char* flag,
                                      std::string& left, std::string& right){
                const size_t eq = text.find('=');
                if(eq == std::string::npos){
                    std::cerr << flag << " expects <...>=<...>, got: " << text << std::endl;
                    return false;
                }
                left = text.substr(0, eq);
                right = text.substr(eq + 1);
                return true;
            };

            if(arg == "-h" || arg == "--help"){
                printUsage(argv[0]);
                exitRequested = true;
                return true;
            }else if(arg == "-l" || arg == "--list"){
                cli.listRequested = true;  // handled after parsing so --api applies
            }else if(arg == "-s" || arg == "--synth"){
                if(const char* v = nextArg()) cli.synthName = v;
            }else if(arg == "-a" || arg == "--api"){
                if(const char* v = nextArg()) cli.host.audioApiName = v;
            }else if(arg == "-d" || arg == "--device"){
                if(const char* v = nextArg()) cli.host.audioDeviceId = std::stoul(v);
            }else if(arg == "-m" || arg == "--midi"){
                if(const char* v = nextArg()) cli.host.midiPortIndex = std::stoi(v);
            }else if(arg == "-r" || arg == "--rate"){
                if(const char* v = nextArg()) cli.host.sampleRate = std::stoul(v);
            }else if(arg == "-b" || arg == "--buffer"){
                if(const char* v = nextArg()) cli.host.bufferFrames = std::stoul(v);
            }else if(arg == "-g" || arg == "--gain"){
                if(const char* v = nextArg()) cli.gain = std::stof(v);
            }else if(arg == "-p" || arg == "--param"){
                if(const char* v = nextArg()){
                    std::string id, value;
                    if(!splitAssignment(v, "--param", id, value)) return false;
                    cli.paramOverrides.emplace_back(id, std::stof(value));
                }
            }else if(arg == "--adc"){
                if(const char* v = nextArg()){
                    std::string channel, id;
                    if(!splitAssignment(v, "--adc", channel, id)) return false;
                    cli.adcMappings.emplace_back(std::stoi(channel), id);
                }
            }else if(arg == "--adc-device"){
                if(const char* v = nextArg()) cli.adcDevice = v;
            }else if(arg == "--enc"){
                if(const char* v = nextArg()){
                    std::string pins, id;
                    if(!splitAssignment(v, "--enc", pins, id)) return false;
                    const size_t comma = pins.find(',');
                    if(comma == std::string::npos){
                        std::cerr << "--enc expects <pinA>,<pinB>=<param id>, got: "
                                  << v << std::endl;
                        return false;
                    }
                    cli.encoderMappings.push_back({
                        static_cast<unsigned int>(std::stoul(pins.substr(0, comma))),
                        static_cast<unsigned int>(std::stoul(pins.substr(comma + 1))),
                        id});
                }
            }else if(arg == "--enc-chip"){
                if(const char* v = nextArg()) cli.encoderChip = v;
            }else if(arg == "--enc-step"){
                if(const char* v = nextArg()) cli.encoderStep = std::stof(v);
            }else if(arg == "-v" || arg == "--verbose"){
                cli.host.verbose = true;
            }else{
                std::cerr << "Unknown option: " << arg << std::endl;
                printUsage(argv[0]);
                return false;
            }
        }
    }catch(const std::exception&){
        std::cerr << "Invalid option value." << std::endl;
        printUsage(argv[0]);
        return false;
    }
    return true;
}

std::unique_ptr<rtsynth::Processor> createSynth(const std::string& name){
    if(name == "sine"){
        return std::make_unique<rtsynth::SineSynthProcessor>();
    }
    if(name == "pd"){
#ifdef RTSYNTH_HAVE_PD
        return std::make_unique<rtsynth::PdSynthProcessor>();
#else
        std::cerr << "This build has no PD synth (external/pd submodule was missing).\n"
                     "Run: git submodule update --init && rebuild." << std::endl;
        return nullptr;
#endif
    }
    std::cerr << "Unknown synth: " << name << " (available: sine, pd)" << std::endl;
    return nullptr;
}

}  // namespace

int main(int argc, char* argv[]){
    CliOptions cli;
    bool exitRequested = false;
    if(!parseArguments(argc, argv, cli, exitRequested)){
        return 1;
    }
    if(exitRequested){
        return 0;
    }
    if(cli.listRequested){
        listDevices(cli.host.audioApiName);
        return 0;
    }

    std::unique_ptr<rtsynth::Processor> synth = createSynth(cli.synthName);
    if(synth == nullptr){
        return 1;
    }
    std::cout << "Instrument: " << synth->name() << std::endl;

    // -g targets the master output whatever the instrument calls it
    if(cli.gain >= 0.0f){
        rtsynth::Parameter* master = synth->parameters().byId("gain");
        if(master == nullptr){
            master = synth->parameters().byId("volume");
        }
        if(master != nullptr){
            master->set(cli.gain);
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

    for(const auto& [id, value] : cli.paramOverrides){
        rtsynth::Parameter* parameter = findParameter(id);
        if(parameter == nullptr){
            return 1;
        }
        parameter->set(value);
    }

    rtsynth::StandaloneHost host(*synth);
    if(!host.start(cli.host)){
        return 1;
    }

    // optional hardware controls: ADC pots (absolute) and/or GPIO rotary
    // encoders (relative), both feeding the same ControlLoop
    rtsynth::Mcp3008Input adc;
    rtsynth::GpioEncoderInput encoders;
    rtsynth::ControlLoop controlLoop(&adc, &encoders);

    if(!cli.adcMappings.empty()){
        if(!adc.open(cli.adcDevice)){
            return 1;
        }
        for(const auto& [channel, id] : cli.adcMappings){
            rtsynth::Parameter* parameter = findParameter(id);
            if(parameter == nullptr){
                return 1;
            }
            controlLoop.addMapping(channel, parameter);
        }
        std::cout << "ADC control: " << adc.name() << " on " << cli.adcDevice
                  << ", " << cli.adcMappings.size() << " mapping(s)" << std::endl;
    }

    if(!cli.encoderMappings.empty()){
        for(const EncoderMapping& mapping : cli.encoderMappings){
            rtsynth::Parameter* parameter = findParameter(mapping.paramId);
            if(parameter == nullptr){
                return 1;
            }
            const int channel = encoders.addEncoder(mapping.pinA, mapping.pinB);
            controlLoop.addRelativeMapping(channel, parameter, cli.encoderStep);
        }
        if(!encoders.open(cli.encoderChip)){
            return 1;
        }
        std::cout << "Encoder control: " << encoders.name() << " on " << cli.encoderChip
                  << ", " << cli.encoderMappings.size() << " mapping(s)" << std::endl;
    }

    if(!cli.adcMappings.empty() || !cli.encoderMappings.empty()){
        controlLoop.start();
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout << "Running. Press Ctrl+C to quit." << std::endl;

    // watches for parameter changes from any source (MIDI CC, pots,
    // encoders); the verbose print below is the console stand-in for a
    // future LCD, which would consume the same watcher from a UI thread
    rtsynth::ParameterWatcher watcher(synth->parameters());

    uint64_t lastXruns = 0;
    uint64_t lastDrops = 0;
    // poll faster in verbose mode so MIDI/parameter prints feel immediate
    const auto pollPeriod = std::chrono::milliseconds(cli.host.verbose? 50 : 500);
    while(g_running.load()){
        std::this_thread::sleep_for(pollPeriod);

        if(cli.host.verbose){
            host.midi().drainMonitor(printMidiEvent);
            watcher.pollChanges(printParameterChange);
        }

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
    encoders.close();
    host.stop();
    return 0;
}
