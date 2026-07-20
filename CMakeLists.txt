cmake_minimum_required(VERSION 3.18)

project(rtsynth CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# --- core library: platform-independent, no RtAudio/RtMidi dependency ------
add_library(rtsynth_core STATIC
    src/synth/SineSynthProcessor.cpp
)
target_include_directories(rtsynth_core PUBLIC src)

# --- optional PD synth: DSP core of the pd VST3 plugin (git submodule) ------
# external/pd is used unmodified; src/vst3shim satisfies its
# #include "pluginterfaces/vst/vsttypes.h" without the VST3 SDK.
set(RTSYNTH_PD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/pd)
if(EXISTS ${RTSYNTH_PD_DIR}/pd.cpp)
    add_library(rtsynth_pd STATIC
        ${RTSYNTH_PD_DIR}/pd.cpp
        ${RTSYNTH_PD_DIR}/eg.cpp
        ${RTSYNTH_PD_DIR}/voice.cpp
        src/synth/PdSynthProcessor.cpp
    )
    target_include_directories(rtsynth_pd PUBLIC src src/vst3shim ${RTSYNTH_PD_DIR})
    target_compile_definitions(rtsynth_pd PUBLIC RTSYNTH_HAVE_PD=1)
    message(STATUS "PD synth enabled (external/pd)")
else()
    message(STATUS "external/pd not found - building without the PD synth. "
                   "Run: git submodule update --init")
endif()

# --- standalone host: RtAudio + RtMidi --------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED IMPORTED_TARGET rtaudio)
pkg_check_modules(RTMIDI REQUIRED IMPORTED_TARGET rtmidi)

add_executable(rtsynth
    src/main.cpp
    src/host/GpioEncoderInput.cpp
    src/host/Mcp3008Input.cpp
    src/host/RtAudioOutput.cpp
    src/host/RtMidiInput.cpp
    src/host/StandaloneHost.cpp
)
target_link_libraries(rtsynth PRIVATE rtsynth_core PkgConfig::RTAUDIO PkgConfig::RTMIDI)
if(TARGET rtsynth_pd)
    target_link_libraries(rtsynth PRIVATE rtsynth_pd)
endif()

# --- offline self-test: needs no audio hardware ------------------------------
enable_testing()
find_package(Threads REQUIRED)
add_executable(rtsynth_selftest tests/selftest.cpp)
target_link_libraries(rtsynth_selftest PRIVATE rtsynth_core Threads::Threads)
if(TARGET rtsynth_pd)
    target_link_libraries(rtsynth_selftest PRIVATE rtsynth_pd)
endif()
add_test(NAME selftest COMMAND rtsynth_selftest)
