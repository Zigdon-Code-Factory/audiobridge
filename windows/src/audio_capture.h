#pragma once
#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>
#include <deque>
#include <vector>
#include <string>

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

class AudioCapture {
public:
    // Callback receives interleaved float stereo samples at 48kHz, 480 samples (10ms)
    using FrameCallback = std::function<void(const float* data, uint32_t frameCount)>;

    AudioCapture();
    ~AudioCapture();

    static std::vector<AudioDeviceInfo> getDevices(bool loopback = true);

    bool initialize(const std::wstring& deviceId = L"", bool loopback = true);
    void cleanup();
    bool start(FrameCallback callback);
    void stop();

    void setOnDeviceInvalidated(std::function<void()> cb) { onDeviceInvalidated_ = cb; }

    uint32_t getSampleRate() const { return sampleRate_; }
    uint32_t getChannels() const { return channels_; }

private:
    void captureThread();
    void resampleAndDeliver(const float* src, uint32_t srcFrames, uint32_t srcChannels, uint32_t srcRate);

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;

    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;
    bool loopback_ = true;
    bool isFloat_ = true;
    uint32_t bitsPerSample_ = 32;

    FrameCallback callback_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    HANDLE captureEvent_ = nullptr; // event-driven WASAPI signaling

    std::function<void()> onDeviceInvalidated_;

    // Resampling accumulation buffer
    std::deque<float> resampleBuf_;
    double resamplePos_ = 0.0;

    // Peak level monitoring
    float peakLevel_ = 0.0f;
    uint64_t peakSampleCount_ = 0;
    static constexpr uint64_t PEAK_REPORT_INTERVAL = 48000 * 5; // every 5 seconds
};
