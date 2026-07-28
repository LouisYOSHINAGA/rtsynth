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
#include "host/DisplayUi.hpp"
#include "host/GpioEncoderInput.hpp"
#include "host/I2cLcd1602.hpp"
#include "host/Mcp3008Input.hpp"
#include "host/ParameterWatcher.hpp"
#include "host/RawMidiInput.hpp"
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
        "  -m, --midi <index>   restrict MIDI input to one sequencer port index\n"
        "                       (default: connect to all ports, e.g. keyboard + CC box)\n"
        "  --midi-raw <dev>     read MIDI straight from a kernel rawmidi device,\n"
        "                       bypassing the ALSA sequencer; repeatable, or 'all'\n"
        "                       (e.g. --midi-raw hw:1,0,0 — ids shown by --list)\n"
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
        "  --lcd <addr>         show the last-changed parameter on a 16x2 I2C LCD\n"
        "                       at this address (e.g. --lcd 0x27; find with i2cdetect)\n"
        "  --lcd-bus <path>     I2C bus of the LCD (default: /dev/i2c-1)\n"
        "  --voices <n>         cap polyphony (fewer voices = less CPU; the pd\n"
        "                       synth is expensive, try 8 or 6 if audio crackles)\n"
        "  -v, --verbose        print received MIDI events, parameter changes and\n"
        "                       DSP load, and show audio backend warnings\n"
        "  --midi-dump          also print the raw MIDI bytes as received, grouped\n"
        "                       per device read (--midi-raw) or per message;\n"
        "                       settles what the device really sent\n"
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
    std::cout << "=== MIDI Input Ports (sequencer) ===" << std::endl;
    const auto ports = midi.listPorts();
    if(ports.empty()){
        std::cout << "  (none)" << std::endl;
    }
    for(size_t i = 0; i < ports.size(); i++){
        std::cout << "  [" << i << "] " << ports[i] << std::endl;
    }

    std::cout << "=== Raw MIDI Devices (--midi-raw) ===" << std::endl;
    const auto rawInputs = rtsynth::RawMidiInput::listInputs();
    if(rawInputs.empty()){
        std::cout << "  (none)" << std::endl;
    }
    for(const auto& [id, name] : rawInputs){
        std::cout << "  " << id << "  " << name << std::endl;
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
    int lcdAddress = -1;               // -1 = no LCD
    std::string lcdBus = "/dev/i2c-1";
    int maxVoices = 0;                 // 0 = instrument default
    bool midiDump = false;
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
            }else if(arg == "--midi-raw"){
                if(const char* v = nextArg()) cli.host.rawMidiDevices.push_back(v);
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
            }else if(arg == "--lcd"){
                if(const char* v = nextArg()){
                    cli.lcdAddress = static_cast<int>(std::stoul(v, nullptr, 0));  // "0x27" ok
                }
            }else if(arg == "--lcd-bus"){
                if(const char* v = nextArg()) cli.lcdBus = v;
            }else if(arg == "--voices"){
                if(const char* v = nextArg()) cli.maxVoices = std::stoi(v);
            }else if(arg == "--midi-dump"){
                cli.midiDump = true;
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

    // "--midi-raw all" opens every raw device, which also covers devices
    // that expose several MIDI cables as separate subdevices (a control
    // surface keyboard typically does)
    if(cli.host.rawMidiDevices.size() == 1 && cli.host.rawMidiDevices[0] == "all"){
        cli.host.rawMidiDevices.clear();
        for(const auto& [id, name] : rtsynth::RawMidiInput::listInputs()){
            std::cout << "Raw MIDI device: " << id << "  " << name << std::endl;
            cli.host.rawMidiDevices.push_back(id);
        }
        if(cli.host.rawMidiDevices.empty()){
            std::cerr << "No raw MIDI device found." << std::endl;
            return 1;
        }
    }

    std::unique_ptr<rtsynth::Processor> synth = createSynth(cli.synthName);
    if(synth == nullptr){
        return 1;
    }
    std::cout << "Instrument: " << synth->name() << std::endl;
    if(cli.maxVoices > 0){
        synth->setMaxVoices(cli.maxVoices);
        std::cout << "Polyphony capped at " << cli.maxVoices << " voices" << std::endl;
    }

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
    if(cli.midiDump){
        host.midi().setRawDumpEnabled(true);
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

    // optional 16x2 I2C LCD showing the last-changed parameter
    rtsynth::I2cLcd1602 lcd;
    rtsynth::DisplayUi displayUi(lcd, synth->parameters());
    if(cli.lcdAddress >= 0){
        if(!lcd.open(cli.lcdBus, static_cast<uint8_t>(cli.lcdAddress))){
            return 1;
        }
        displayUi.showStatus(synth->name(), "ready");
        displayUi.start();
        std::cout << "LCD: 16x2 on " << cli.lcdBus << " addr 0x"
                  << std::hex << cli.lcdAddress << std::dec << std::endl;
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
    uint64_t lastDeferrals = 0;
    uint64_t lastUndecoded = 0;
    int lastVoices = -1;
    int loadTicks = 0;
    uint64_t lastReadErrors = 0;
    bool schedulingReported = false;
    // poll faster in verbose mode so MIDI/parameter prints feel immediate
    const auto pollPeriod = std::chrono::milliseconds(cli.host.verbose? 50 : 500);
    while(g_running.load()){
        std::this_thread::sleep_for(pollPeriod);

        // report once whether the audio thread really got realtime
        // scheduling — silently missing rtprio permission is the most
        // common cause of crackling on a Raspberry Pi
        if(!schedulingReported && host.audio().audioThreadPolicy() >= 0){
            schedulingReported = true;
            if(host.audio().audioThreadIsRealtime()){
                std::cout << "Audio thread: realtime scheduling active" << std::endl;
            }else{
                std::cerr <<
                    "[warning] audio thread did NOT get realtime priority — crackling\n"
                    "          under load is expected. Fix: add your user to the 'audio'\n"
                    "          group and create /etc/security/limits.d/audio.conf with:\n"
                    "            @audio - rtprio 95\n"
                    "            @audio - memlock unlimited\n"
                    "          then log out and back in." << std::endl;
            }
        }

        if(cli.midiDump){
            // One line per read() from the device, so a burst that is
            // missing a message is visibly different from a burst that
            // never arrived at all.
            bool open = false;
            host.midi().drainRawDump([&open](uint8_t byte, bool startsGroup){
                if(startsGroup){
                    if(open){
                        std::cout << std::endl;
                    }
                    std::cout << "[read]";
                    open = true;
                }
                std::printf(" %02X", byte);
            });
            if(open){
                std::cout << std::endl;
            }
        }

        if(cli.host.verbose){
            host.midi().drainMonitor(printMidiEvent);
            watcher.pollChanges(printParameterChange);

            // DSP load: > ~0.8 means the render barely fits the deadline and
            // crackling is a CPU problem (lower --voices / raise --buffer).
            // Summarized once a second so it doesn't drown the MIDI trace.
            if(++loadTicks >= 20){
                loadTicks = 0;
                const float peak = host.audio().peakLoad();
                std::printf("[load] %.0f%% peak / %.0f%% now, %d voices%s\n",
                            peak * 100.0f, host.audio().currentLoad() * 100.0f,
                            synth->activeVoiceCount(),
                            (peak > 0.8f)? "  <-- too high, lower --voices" : "");
                host.audio().resetPeakLoad();
            }

            // stuck-voice gauge: a count pinned at the maximum while no key
            // is held means voices never end (lost note-offs / stalled EGs)
            const int voices = synth->activeVoiceCount();
            if(voices != lastVoices){
                std::cout << "[voices] " << voices << " active" << std::endl;
                lastVoices = voices;
            }
        }

        // report problems from the main thread, never from RT threads
        const uint64_t xruns = host.audio().xrunCount();
        if(xruns != lastXruns){
            std::cerr << "[warning] audio under/overflow (total: " << xruns << ")" << std::endl;
            lastXruns = xruns;
        }
        const uint64_t drops = host.midi().droppedCount();
        if(drops != lastDrops){
            std::cerr << "[warning] MIDI events dropped (total: " << drops << ")" << std::endl;
            lastDrops = drops;
        }
        const uint64_t deferrals = host.midiDeferralCount();
        if(deferrals != lastDeferrals){
            std::cerr << "[warning] MIDI backlog deferred to next block (total: "
                      << deferrals << ")" << std::endl;
            lastDeferrals = deferrals;
        }
        const uint64_t undecoded = host.midi().undecodedCount();
        if(undecoded != lastUndecoded){
            std::cerr << "[warning] undecodable MIDI messages (total: " << undecoded
                      << ")" << std::endl;
            lastUndecoded = undecoded;
        }
        const uint64_t readErrors = host.midi().readErrorCount();
        if(readErrors != lastReadErrors){
            std::cerr << "[warning] MIDI device read errors — the kernel buffer"
                         " overran and bytes were lost (total: " << readErrors << ")"
                      << std::endl;
            lastReadErrors = readErrors;
        }
    }

    std::cout << "\nShutting down." << std::endl;
    displayUi.stop();
    controlLoop.stop();
    encoders.close();
    host.stop();
    return 0;
}
