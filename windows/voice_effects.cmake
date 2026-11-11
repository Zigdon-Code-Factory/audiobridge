# Native CPU inference. Runtime and models are copied beside the executable.
FetchContent_Declare(onnxruntime
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip
    URL_HASH SHA256=0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd
)
FetchContent_MakeAvailable(onnxruntime)
add_library(audiobridge_voice STATIC src/voice_effects.cpp)
target_compile_features(audiobridge_voice PUBLIC cxx_std_17)
target_include_directories(audiobridge_voice PUBLIC src PRIVATE "${onnxruntime_SOURCE_DIR}/include")
target_compile_definitions(audiobridge_voice PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(audiobridge_voice PUBLIC "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(VOICE_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/voice-models")
add_custom_target(voice_models
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/voice_models.py" "${VOICE_MODEL_DIR}"
    VERBATIM
)
function(copy_voice_runtime target)
    add_dependencies(${target} voice_models)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${VOICE_MODEL_DIR}" "$<TARGET_FILE_DIR:${target}>/voice-models"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/onnxruntime"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${onnxruntime_SOURCE_DIR}/LICENSE" "${onnxruntime_SOURCE_DIR}/ThirdPartyNotices.txt"
            "$<TARGET_FILE_DIR:${target}>/licenses/onnxruntime"
        VERBATIM
    )
endfunction()
