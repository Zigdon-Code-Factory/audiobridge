#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>
#include <vector>
#include <string>
#include <deque>

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

class AudioCapture {
public:
    using FrameCallback = std::function<void(const float* data, uint32_t frameCount)>;

    AudioCapture();
    ~AudioCapture();

    static std::vector<AudioDeviceInfo> getDevices();

    bool initialize(const std::wstring& deviceId = L"");
    void cleanup();
    bool start(FrameCallback callback);
    void stop();

    void setOnDeviceInvalidated(std::function<void()> cb) { onDeviceInvalidated_ = cb; }

    uint32_t getSampleRate() const { return sampleRate_; }
    uint32_t getChannels() const { return channels_; }

private:
    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;

    FrameCallback callback_;
    std::atomic<bool> running_{false};
    std::function<void()> onDeviceInvalidated_;

    uint32_t deviceId_ = 0; // CoreAudio AudioDeviceID
    void* audioUnit_ = nullptr; // AudioComponentInstance (opaque to avoid header dep)

    // Resampling buffer for non-48kHz devices
    std::deque<float> resampleBuf_;
    double resamplePos_ = 0.0;
    uint32_t deviceSampleRate_ = 0;
    uint32_t deviceChannels_ = 0;

    static int inputCallback(void* inRefCon, unsigned int inActionFlags,
                             const void* inTimeStamp, unsigned int inBusNumber,
                             unsigned int inNumberFrames, void* ioData);
};
