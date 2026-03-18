cmake_minimum_required(VERSION 3.20)
project(AudioBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch libopus
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

# Fetch webview
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG        0.12.0
)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(webview)

# Find PulseAudio
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED libpulse libpulse-simple)

add_executable(audiobridge
    src/main.cpp
    src/audio_capture.cpp
    src/audio_render.cpp
    src/opus_encoder.cpp
    src/opus_decoder.cpp
    src/network.cpp
    src/gui.cpp
)

target_include_directories(audiobridge PRIVATE
    ${opus_SOURCE_DIR}/include
    ${PULSE_INCLUDE_DIRS}
    src
)

target_link_libraries(audiobridge PRIVATE
    opus
    webview::core
    ${PULSE_LIBRARIES}
    pthread
)

target_compile_definitions(audiobridge PRIVATE
    NOMINMAX
)
