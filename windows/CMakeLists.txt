cmake_minimum_required(VERSION 3.20)
project(AudioBridge VERSION 0.2.0 LANGUAGES C CXX)

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

include(voice_effects.cmake)

add_library(audiobridge_update_support STATIC src/update_support.cpp)
target_compile_definitions(audiobridge_update_support PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(audiobridge_update_support PUBLIC bcrypt winhttp windowsapp ole32)
add_executable(audiobridge_updater WIN32 src/update_helper.cpp)
set_target_properties(audiobridge_updater PROPERTIES OUTPUT_NAME "audiobridge-updater")
target_link_libraries(audiobridge_updater PRIVATE audiobridge_update_support shell32)

add_executable(audiobridge WIN32
    src/main.cpp
    src/audio_capture.cpp
    src/audio_render.cpp
    src/virtual_microphone.cpp
    src/opus_encoder.cpp
    src/opus_decoder.cpp
    src/network.cpp
    src/gui.cpp
    src/app_updater.cpp
    src/version.rc
    ../common/dtls_session.cpp
)

target_include_directories(audiobridge PRIVATE
    ${opus_SOURCE_DIR}/include
    ${mbedtls_SOURCE_DIR}/include
    ../common
    src
)

target_link_libraries(audiobridge PRIVATE
    audiobridge_voice
    audiobridge_update_support
    opus
    webview::core
    ws2_32
    qwave
    ole32
    avrt
    iphlpapi
    windowsapp
    advapi32
    shell32
    mbedtls
    mbedcrypto
    mbedx509
)

copy_voice_runtime(audiobridge)
add_dependencies(audiobridge audiobridge_updater)

enable_testing()
add_executable(updater_test ../tests/updater_test.cpp src/app_updater.cpp)
target_include_directories(updater_test PRIVATE src ../common)
target_link_libraries(updater_test PRIVATE audiobridge_update_support)
target_compile_options(updater_test PRIVATE /UNDEBUG)
add_test(NAME updater COMMAND updater_test)
set_tests_properties(updater PROPERTIES TIMEOUT 90)
add_executable(voice_effects_test ../tests/voice_effects_test.cpp)
target_link_libraries(voice_effects_test PRIVATE audiobridge_voice)
target_compile_options(voice_effects_test PRIVATE /UNDEBUG)
copy_voice_runtime(voice_effects_test)
add_test(NAME voice_effects COMMAND voice_effects_test "$<TARGET_FILE_DIR:voice_effects_test>/voice-models")
set_tests_properties(voice_effects PROPERTIES TIMEOUT 60)
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME voice_ui COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/voice_ui_test.cjs")
endif()

add_executable(audiobridge_mic_service src/mic_service.cpp)
target_compile_definitions(audiobridge_mic_service PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(audiobridge_mic_service PRIVATE swdevice cfgmgr32 advapi32)
set_property(TARGET audiobridge_mic_service PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_compile_definitions(audiobridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
)
