# Native CPU + DirectML GPU inference. Runtime and models are copied beside the executable.
FetchContent_Declare(onnxruntime
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg
    URL_HASH SHA256=57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681
)
FetchContent_Declare(directml
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg
    URL_HASH SHA256=4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9
)
FetchContent_MakeAvailable(onnxruntime directml)
add_library(audiobridge_voice STATIC src/voice_effects.cpp)
target_compile_features(audiobridge_voice PUBLIC cxx_std_17)
target_include_directories(audiobridge_voice PUBLIC src PRIVATE
    "${onnxruntime_SOURCE_DIR}/build/native/include"
    "${directml_SOURCE_DIR}/include"
)
target_compile_definitions(audiobridge_voice PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN AUDIOBRIDGE_HAS_DIRECTML=1)
target_link_libraries(audiobridge_voice PUBLIC "${onnxruntime_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime.lib" dxgi)
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
            "${onnxruntime_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime.dll"
            "${onnxruntime_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
            "${directml_SOURCE_DIR}/bin/x64-win/DirectML.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${VOICE_MODEL_DIR}" "$<TARGET_FILE_DIR:${target}>/voice-models"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/onnxruntime"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${onnxruntime_SOURCE_DIR}/LICENSE" "${onnxruntime_SOURCE_DIR}/ThirdPartyNotices.txt"
            "$<TARGET_FILE_DIR:${target}>/licenses/onnxruntime"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/directml"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${directml_SOURCE_DIR}/LICENSE.txt" "${directml_SOURCE_DIR}/ThirdPartyNotices.txt"
            "$<TARGET_FILE_DIR:${target}>/licenses/directml"
        VERBATIM
    )
endfunction()
