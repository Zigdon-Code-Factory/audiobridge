cmake_minimum_required(VERSION 3.18)
project(native_audio)

set(CMAKE_CXX_STANDARD 17)

# Fetch Oboe
include(FetchContent)
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com/google/oboe.git
    GIT_TAG 1.9.0
)
FetchContent_MakeAvailable(oboe)

# Fetch Opus
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
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

# Path to common/ directory
set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../common)

add_library(native_audio SHARED
    native_audio.cpp
    ${COMMON_DIR}/dtls_session.cpp
)

target_link_libraries(native_audio
    oboe
    opus
    mbedtls
    mbedcrypto
    mbedx509
    android
    log
)

target_include_directories(native_audio PRIVATE
    ${opus_SOURCE_DIR}/include
    ${COMMON_DIR}
    ${mbedtls_SOURCE_DIR}/include
)
