#pragma once
#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

class AudioRender {
public:
    using DeviceInvalidatedCallback = std::function<void()>;

    AudioRender();
    ~AudioRender();

    static std::vector<std::pair<std::wstring, std::wstring>> getDevices();

    bool initialize(const std::wstring& deviceId = L"");
    void cleanup();
    bool start();
    void stop();

    // Push 48kHz mono float PCM data (from Opus decoder)
    void pushAudio(const float* data, uint32_t frameCount);

    void setOnDeviceInvalidated(DeviceInvalidatedCallback cb) { onDeviceInvalidated_ = cb; }

private:
    void renderThread();

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;

    // Device mix format properties
    uint32_t deviceSampleRate_ = 0;
    uint32_t deviceChannels_ = 0;
    uint32_t deviceBitsPerSample_ = 0;
    bool deviceIsFloat_ = false;

    // Input format (what pushAudio receives)
    static constexpr uint32_t INPUT_SAMPLE_RATE = 48000;
    static constexpr uint32_t INPUT_CHANNELS = 1; // mono from mic

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex bufferMutex_;
    // Internal buffer stores audio in device format (interleaved, device channels, device sample rate)
    std::vector<float> audioBuffer_;  // always float internally, convert on write to WASAPI

    bool comInitialized_ = false;

    DeviceInvalidatedCallback onDeviceInvalidated_;
};
