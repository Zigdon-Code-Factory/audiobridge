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

add_library(native_audio SHARED native_audio.cpp)

target_link_libraries(native_audio
    oboe
    opus
    android
    log
)

target_include_directories(native_audio PRIVATE
    ${opus_SOURCE_DIR}/include
)
