cmake_minimum_required(VERSION 3.20)
project(AudioBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch libopus
include(FetchContent)
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

add_executable(audiobridge WIN32
    src/main.cpp
    src/audio_capture.cpp
    src/opus_encoder.cpp
    src/network.cpp
    src/gui.cpp
)

target_include_directories(audiobridge PRIVATE
    ${opus_SOURCE_DIR}/include
    src
)

target_link_libraries(audiobridge PRIVATE
    opus
    ws2_32
    ole32
    avrt
    iphlpapi
    comctl32
    uxtheme
    gdi32
    dwmapi
)

target_compile_definitions(audiobridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
)
