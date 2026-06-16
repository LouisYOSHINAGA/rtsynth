cmake_minimum_required(VERSION 3.25)

project(RtTestSynth CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED IMPORTED_TARGET rtaudio)
pkg_check_modules(RTMIDI REQUIRED IMPORTED_TARGET rtmidi)

add_executable(rtsynth rcv.cpp gen.cpp)

target_link_libraries(rtsynth PRIVATE PkgConfig::RTAUDIO PkgConfig::RTMIDI)

target_compile_options(rtsynth PRIVATE -O2)