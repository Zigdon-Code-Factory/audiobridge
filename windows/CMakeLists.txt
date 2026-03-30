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

# Fetch mbedTLS
FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        v3.6.2
)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mbedtls)

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

add_executable(audiobridge WIN32
    src/main.cpp
    src/audio_capture.cpp
    src/audio_render.cpp
    src/opus_encoder.cpp
    src/opus_decoder.cpp
    src/network.cpp
    src/gui.cpp
    ../common/dtls_session.cpp
)

target_include_directories(audiobridge PRIVATE
    ${opus_SOURCE_DIR}/include
    ${mbedtls_SOURCE_DIR}/include
    ../common
    src
)

target_link_libraries(audiobridge PRIVATE
    opus
    webview::core
    ws2_32
    ole32
    avrt
    iphlpapi
    windowsapp
    mbedtls
    mbedcrypto
    mbedx509
)

target_compile_definitions(audiobridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
)
