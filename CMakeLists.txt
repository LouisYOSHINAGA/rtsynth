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

# --- standalone host: RtAudio + RtMidi --------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED IMPORTED_TARGET rtaudio)
pkg_check_modules(RTMIDI REQUIRED IMPORTED_TARGET rtmidi)

add_executable(rtsynth
    src/main.cpp
    src/host/RtAudioOutput.cpp
    src/host/RtMidiInput.cpp
    src/host/StandaloneHost.cpp
)
target_link_libraries(rtsynth PRIVATE rtsynth_core PkgConfig::RTAUDIO PkgConfig::RTMIDI)

# --- offline self-test: needs no audio hardware ------------------------------
enable_testing()
add_executable(rtsynth_selftest tests/selftest.cpp)
target_link_libraries(rtsynth_selftest PRIVATE rtsynth_core)
add_test(NAME selftest COMMAND rtsynth_selftest)
